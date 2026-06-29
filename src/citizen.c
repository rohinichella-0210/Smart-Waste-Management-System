#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "citizen.h"
#include "types.h"
#include "bins.h"
#include "simulation.h"   /* for sim_emergency, sim_log */
#include "routes.h"       /* for routes_emergency, route_find */
#include "vehicles.h"     /* for vehicle_find, vehicle_deduct_fuel, vehicle_unload */
#include "storage.h"      /* for storage_save_all */

/* ── Globals ─────────────────────────────────────────────── */
CitizenReport g_citizen_reports[MAX_CITIZEN_REPORTS];
int           g_citizen_report_count    = 0;
int           g_citizen_report_next_id  = 1;

/* ── Weight table per report type ────────────────────────── */
/* Higher weight = stronger priority boost per report */
static float type_weights[4] = {
    3.0f,   /* OVERFLOW   — almost certainly needs immediate collection */
    4.0f,   /* VANDALIZED — safety concern, flag quickly */
    5.0f,   /* HAZARD     — highest urgency, escalate fast */
    2.0f,   /* FULL       — bin full but not overflowing */
};

/* Reason strings */
static const char *type_names[4] = {
    "overflow", "vandalized", "hazard", "full"
};

/* ── Timestamp helper ────────────────────────────────────── */
static void now_str(char *buf, int sz) {
    time_t t = time(NULL);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", localtime(&t));
}

/* ── Recompute citizen_priority for one bin ─────────────── */
/*
 * citizen_priority scale:
 *   0 = no reports
 *   1 = 1–2 low-weight reports  (weight sum < 5)
 *   2 = urgent                  (weight sum >= 5 OR any HAZARD/VANDAL report)
 *
 * Effective bin priority = max(fill-based priority, citizen_priority)
 */
void citizen_recompute_priority(int bin_idx) {
    if (bin_idx < 0 || bin_idx >= g_bin_count) return;
    Bin *b = &g_bins[bin_idx];

    float weight_sum   = 0.0f;
    int   has_urgent   = 0;
    int   open_reports = 0;

    for (int i = 0; i < g_citizen_report_count; i++) {
        CitizenReport *r = &g_citizen_reports[i];
        if (r->bin_id != b->bin_id || r->resolved) continue;
        open_reports++;
        weight_sum += r->weight;
        if (r->report_type == REPORT_TYPE_VANDALIZED ||
            r->report_type == REPORT_TYPE_HAZARD)
            has_urgent = 1;
    }

    /* Compute citizen_priority */
    int cp = 0;
    if (open_reports > 0) {
        if (has_urgent || weight_sum >= 5.0f)
            cp = 2;
        else
            cp = 1;
    }

    b->citizen_priority = cp;

    /* Effective bin priority = max of fill-based and citizen */
    int fill_priority = b->priority;
    if (cp > fill_priority) {
        b->priority = cp;
        sim_log("WARN","CITIZEN",
            "Bin %d priority raised to %d by citizen reports (w=%.1f)",
            b->bin_id, b->priority, weight_sum);
    }
}

/* ── Submit a report ─────────────────────────────────────── */
int citizen_report(int bin_id, int report_type,
                   const char *description,
                   const char *reporter_name,
                   int auto_dispatch)
{
    /* Validate */
    if (g_citizen_report_count >= MAX_CITIZEN_REPORTS) return -1;
    if (report_type < 0 || report_type > 3) return -2;

    int bin_idx = bin_find(bin_id);
    if (bin_idx < 0) return -3;   /* bin does not exist */

    Bin *b = &g_bins[bin_idx];

    /* Build record */
    CitizenReport *r = &g_citizen_reports[g_citizen_report_count];
    memset(r, 0, sizeof(*r));

    r->report_id   = g_citizen_report_next_id++;
    r->bin_id      = bin_id;
    r->report_type = report_type;
    r->weight      = type_weights[report_type];
    strncpy(r->reason, type_names[report_type], 31);

    if (description && description[0])
        strncpy(r->description, description, 127);
    else
        strncpy(r->description, "(no description)", 127);

    if (reporter_name && reporter_name[0])
        strncpy(r->reporter_name, reporter_name, 47);
    else
        strncpy(r->reporter_name, "Anonymous", 47);

    now_str(r->timestamp, sizeof(r->timestamp));
    r->dispatched = 0;
    r->resolved   = 0;

    g_citizen_report_count++;

    /* Update bin counters */
    b->citizen_report_count++;
    strncpy(b->last_report_reason, r->reason,  31);
    strncpy(b->last_report_time,   r->timestamp, 19);

    /* Vandalized flag */
    if (report_type == REPORT_TYPE_VANDALIZED)
        b->is_vandalized = 1;

    /* Recompute priority */
    citizen_recompute_priority(bin_idx);

    sim_log("EVENT","CITIZEN",
        "Report #%d: Bin %d [%s] '%s' by %s — weight=%.1f citizen_priority=%d",
        r->report_id, bin_id, b->location,
        r->reason, r->reporter_name, r->weight, b->citizen_priority);

    /* ── Auto-dispatch logic ── */
    /*
     * ALWAYS dispatch if auto_dispatch=1 — any citizen report triggers
     * immediate vehicle assignment and route creation.
     * Vehicles are dispatched regardless of fill-level priority.
     */
    if (auto_dispatch && !b->collected_today) {
        sim_log("EVENT","CITIZEN",
            "Auto-dispatch triggered for Bin %d [%s] reason=%s",
            bin_id, b->location, r->reason);

        EmergencyResult er = routes_emergency(bin_id, r->reason);
        r->dispatched = er.success ? 1 : 0;

        if (er.success) {
            sim_log("EVENT","CITIZEN",
                "Emergency V%d dispatched to Bin %d (route %d, %.2fkm)",
                er.vehicle_id, bin_id, er.route_id, er.distance);

            /* Immediately execute: mark bin collected, reset fill/priority,
             * complete the route, and resolve this report — so simulation
             * and emergency dispatch won't double-process it. */
            int ri = route_find(er.route_id);
            if (ri >= 0) {
                Route *rt = &g_routes[ri];
                int vi = vehicle_find(rt->vehicle_id);
                if (vi >= 0) {
                    vehicle_deduct_fuel(rt->vehicle_id, rt->total_distance);
                    vehicle_unload(rt->vehicle_id);
                    g_vehicles[vi].assigned_route = 0;
                    rt->completed = 1;
                    time_t now_t = time(NULL);
                    strftime(rt->completed_at, sizeof(rt->completed_at),
                             "%Y-%m-%d %H:%M:%S", localtime(&now_t));
                }
            }

            b->collected_today = 1;
            b->fill_level      = 0;
            b->priority        = 0;
            b->citizen_priority = 0;
            time_t now_t = time(NULL);
            strftime(b->last_collected, sizeof(b->last_collected),
                     "%Y-%m-%d %H:%M:%S", localtime(&now_t));

            /* Resolve the report so it won't show as open */
            r->resolved = 1;
            citizen_recompute_priority(bin_idx);

            storage_save_all();
        } else {
            sim_log("WARN","CITIZEN",
                "Dispatch failed for Bin %d: %s", bin_id, er.message);
        }

        citizen_save();
        return r->report_id;
    }

    citizen_save();
    return r->report_id;
}

/* ── Resolve a report ────────────────────────────────────── */
int citizen_resolve(int report_id) {
    for (int i = 0; i < g_citizen_report_count; i++) {
        CitizenReport *r = &g_citizen_reports[i];
        if (r->report_id == report_id) {
            r->resolved = 1;
            int bin_idx = bin_find(r->bin_id);
            if (bin_idx >= 0) {
                citizen_recompute_priority(bin_idx);
                /* Clear vandalized if resolved */
                if (r->report_type == REPORT_TYPE_VANDALIZED)
                    g_bins[bin_idx].is_vandalized = 0;
            }
            citizen_save();
            sim_log("INFO","CITIZEN","Report #%d resolved (Bin %d)",
                report_id, r->bin_id);
            return 0;
        }
    }
    return -1;
}

/* ── JSON: all reports or last N ─────────────────────────── */
char *citizen_reports_json(int last_n) {
    int start = 0;
    if (last_n > 0 && g_citizen_report_count > last_n)
        start = g_citizen_report_count - last_n;
    int cnt = g_citizen_report_count - start;

    int sz = 128 + cnt * 320;
    char *buf = malloc(sz);
    if (!buf) return NULL;
    int p = snprintf(buf, sz, "[");

    for (int i = start; i < g_citizen_report_count; i++) {
        CitizenReport *r = &g_citizen_reports[i];

        /* Lookup bin location for display */
        int bi = bin_find(r->bin_id);
        const char *loc = (bi >= 0) ? g_bins[bi].location : "unknown";

        p += snprintf(buf+p, sz-p,
            "%s{"
            "\"report_id\":%d,"
            "\"bin_id\":%d,"
            "\"location\":\"%s\","
            "\"report_type\":%d,"
            "\"reason\":\"%s\","
            "\"description\":\"%s\","
            "\"reporter_name\":\"%s\","
            "\"timestamp\":\"%s\","
            "\"weight\":%.1f,"
            "\"dispatched\":%s,"
            "\"resolved\":%s"
            "}",
            (i > start) ? "," : "",
            r->report_id, r->bin_id, loc,
            r->report_type, r->reason,
            r->description, r->reporter_name,
            r->timestamp, r->weight,
            r->dispatched ? "true" : "false",
            r->resolved   ? "true" : "false");

        if (p > sz - 340) break;
    }

    snprintf(buf+p, sz-p, "]");
    return buf;
}

/* ── JSON: reports for one bin ───────────────────────────── */
char *citizen_bin_reports_json(int bin_id) {
    int sz = 128 + 50 * 320;
    char *buf = malloc(sz);
    if (!buf) return NULL;
    int p = snprintf(buf, sz, "[");
    int first = 1;

    for (int i = 0; i < g_citizen_report_count; i++) {
        CitizenReport *r = &g_citizen_reports[i];
        if (r->bin_id != bin_id) continue;
        p += snprintf(buf+p, sz-p,
            "%s{"
            "\"report_id\":%d,"
            "\"reason\":\"%s\","
            "\"description\":\"%s\","
            "\"reporter_name\":\"%s\","
            "\"timestamp\":\"%s\","
            "\"weight\":%.1f,"
            "\"dispatched\":%s,"
            "\"resolved\":%s"
            "}",
            first ? "" : ",",
            r->report_id, r->reason,
            r->description, r->reporter_name,
            r->timestamp, r->weight,
            r->dispatched ? "true" : "false",
            r->resolved   ? "true" : "false");
        first = 0;
        if (p > sz - 340) break;
    }

    snprintf(buf+p, sz-p, "]");
    return buf;
}

/* ── JSON: summary stats ─────────────────────────────────── */
char *citizen_summary_json(void) {
    int total = g_citizen_report_count;
    int open  = 0, dispatched = 0, resolved = 0;
    int overflow_r = 0, vandal_r = 0, hazard_r = 0, full_r = 0;

    for (int i = 0; i < total; i++) {
        CitizenReport *r = &g_citizen_reports[i];
        if (!r->resolved)  open++;
        if (r->dispatched) dispatched++;
        if (r->resolved)   resolved++;
        switch (r->report_type) {
            case REPORT_TYPE_OVERFLOW:   overflow_r++; break;
            case REPORT_TYPE_VANDALIZED: vandal_r++;   break;
            case REPORT_TYPE_HAZARD:     hazard_r++;   break;
            case REPORT_TYPE_FULL:       full_r++;     break;
        }
    }

    /* Count vandalized bins */
    int vandalized_bins = 0;
    for (int i = 0; i < g_bin_count; i++)
        if (g_bins[i].is_vandalized) vandalized_bins++;

    char *buf = malloc(512);
    if (!buf) return NULL;
    snprintf(buf, 512,
        "{"
        "\"total\":%d,"
        "\"open\":%d,"
        "\"resolved\":%d,"
        "\"dispatched\":%d,"
        "\"by_type\":{"
          "\"overflow\":%d,"
          "\"vandalized\":%d,"
          "\"hazard\":%d,"
          "\"full\":%d"
        "},"
        "\"vandalized_bins\":%d"
        "}",
        total, open, resolved, dispatched,
        overflow_r, vandal_r, hazard_r, full_r,
        vandalized_bins);
    return buf;
}

/* ── Persist ─────────────────────────────────────────────── */
#define CITIZEN_FILE "data/citizen_reports.csv"

void citizen_save(void) {
    FILE *f = fopen(CITIZEN_FILE, "w");
    if (!f) return;
    fprintf(f, "report_id,bin_id,report_type,reason,description,reporter_name,"
               "timestamp,dispatched,resolved,weight\n");
    for (int i = 0; i < g_citizen_report_count; i++) {
        CitizenReport *r = &g_citizen_reports[i];
        fprintf(f, "%d,%d,%d,%s,%s,%s,%s,%d,%d,%.2f\n",
            r->report_id, r->bin_id, r->report_type,
            r->reason, r->description, r->reporter_name,
            r->timestamp, r->dispatched, r->resolved, r->weight);
    }
    fclose(f);
}

void citizen_load(void) {
    FILE *f = fopen(CITIZEN_FILE, "r");
    if (!f) return;
    char line[512];
    fgets(line, sizeof(line), f); /* skip header */
    g_citizen_report_count = 0;
    while (fgets(line, sizeof(line), f) &&
           g_citizen_report_count < MAX_CITIZEN_REPORTS) {
        CitizenReport *r = &g_citizen_reports[g_citizen_report_count];
        memset(r, 0, sizeof(*r));
        /* parse: report_id,bin_id,type,reason,description,reporter,ts,dispatched,resolved,weight */
        if (sscanf(line, "%d,%d,%d,%31[^,],%127[^,],%47[^,],%19[^,],%d,%d,%f",
            &r->report_id, &r->bin_id, &r->report_type,
            r->reason, r->description, r->reporter_name,
            r->timestamp, &r->dispatched, &r->resolved, &r->weight) >= 9)
        {
            if (r->report_id >= g_citizen_report_next_id)
                g_citizen_report_next_id = r->report_id + 1;
            g_citizen_report_count++;
        }
    }
    fclose(f);

    /* Replay open reports into bin priorities */
    for (int i = 0; i < g_bin_count; i++)
        citizen_recompute_priority(i);
}

void citizen_init(void) {
    citizen_load();
}
