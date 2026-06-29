#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "heatmap.h"
#include "types.h"

ZoneHeat g_zone_heat[64];
int      g_zone_heat_count = 0;

/* ── Find or create zone entry ───────────────────────────── */
static ZoneHeat *zone_find_or_create(const char *zone) {
    for (int i = 0; i < g_zone_heat_count; i++)
        if (strcmp(g_zone_heat[i].zone, zone) == 0)
            return &g_zone_heat[i];
    if (g_zone_heat_count >= 64) return NULL;
    ZoneHeat *z = &g_zone_heat[g_zone_heat_count++];
    memset(z, 0, sizeof(*z));
    strncpy(z->zone, zone, 15);
    strncpy(z->heat_level, "green", 7);
    return z;
}

/* ── Rebuild heatmap from live bin data ──────────────────── */
void heatmap_update(void) {
    /* Reset fill-rate averages (keep overflow counts — they are cumulative) */
    for (int i = 0; i < g_zone_heat_count; i++)
        g_zone_heat[i].avg_fill_rate = 0;

    /* Accumulate fill rates per zone */
    int   zone_bin_count[64] = {0};
    for (int i = 0; i < g_bin_count; i++) {
        Bin *b = &g_bins[i];
        ZoneHeat *z = zone_find_or_create(b->zone);
        if (!z) continue;
        int idx = (int)(z - g_zone_heat);
        z->avg_fill_rate += b->fill_rate;
        zone_bin_count[idx]++;
        /* Record overflow if this bin is overflow-priority and not yet collected */
        if (b->priority == 3 && !b->collected_today)
            z->overflow_count++;
    }

    /* Average fill rates */
    for (int i = 0; i < g_zone_heat_count; i++) {
        if (zone_bin_count[i] > 0)
            g_zone_heat[i].avg_fill_rate /= zone_bin_count[i];

        /* zone_score = cumulative overflows + avg fill rate */
        ZoneHeat *z = &g_zone_heat[i];
        z->zone_score = (float)z->overflow_count + z->avg_fill_rate;

        /* colour thresholds — tuned to realistic bin data:
           zone_score = overflow_count + avg_fill_rate (%/day)
           With fill rates 8-18%/day, zones with avg >=12 hit yellow quickly */
        if      (z->zone_score >= 15.0f) strncpy(z->heat_level, "red",    7);
        else if (z->zone_score >=  8.0f) strncpy(z->heat_level, "yellow", 7);
        else                             strncpy(z->heat_level, "green",  7);
    }
}

/* ── Overflow event ──────────────────────────────────────── */
void heatmap_record_overflow(const char *zone) {
    ZoneHeat *z = zone_find_or_create(zone);
    if (z) z->overflow_count++;
}

/* ── JSON output ─────────────────────────────────────────── */
char *heatmap_json(void) {
    heatmap_update();
    /* rough estimate: 120 bytes per zone */
    int sz = 64 + g_zone_heat_count * 140;
    char *buf = malloc(sz);
    if (!buf) return NULL;
    int p = snprintf(buf, sz, "[");
    for (int i = 0; i < g_zone_heat_count; i++) {
        ZoneHeat *z = &g_zone_heat[i];
        p += snprintf(buf+p, sz-p,
            "%s{\"zone\":\"%s\",\"overflow_count\":%d,"
            "\"avg_fill_rate\":%.1f,\"zone_score\":%.1f,"
            "\"heat_level\":\"%s\"}",
            i ? "," : "",
            z->zone, z->overflow_count,
            z->avg_fill_rate, z->zone_score,
            z->heat_level);
    }
    snprintf(buf+p, sz-p, "]");
    return buf;
}
