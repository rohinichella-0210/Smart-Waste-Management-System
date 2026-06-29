#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <time.h>
#include <stdlib.h>
#include "types.h"
#include "vehicles.h"
#include "storage.h"
#include "simulation.h"

Vehicle g_vehicles[MAX_VEHICLES];
int     g_vehicle_count = 0;

int vehicles_init(void) { return 0; }

int vehicle_find(int id) {
    for (int i = 0; i < g_vehicle_count; i++)
        if (g_vehicles[i].vehicle_id == id) return i;
    return -1;
}

/*
 * Dynamic vehicle assignment — truly fair distribution.
 *
 * Hard constraints (disqualify immediately):
 *   - Not available or already assigned or needs maintenance
 *   - Hazardous waste requires hazard_capable vehicle
 *   - Non-hazardous waste must NOT be assigned to hazardous-only vehicles
 *     (prevents V9 monopoly: hazardous truck should only handle hazardous)
 *   - Insufficient load capacity (slack < 0)
 *   - Insufficient fuel for route
 *
 * Soft scoring (lower is better):
 *   +0..100   capacity fit: prefer vehicles with adequate but not excessive capacity
 *   +0..200   zone match bonus: strongly prefer same-zone vehicles
 *   +0..100   fuel adequacy: prefer vehicles with more fuel headroom
 *   +0..50    load balance: prefer vehicles with lower current_load (fairness)
 *   +0..30    idle-time: prefer vehicles that have driven less today (fairness)
 *
 * This scoring naturally distributes across V1-V10 instead of always picking V9.
 */
int vehicle_find_best(const char *zone, const char *waste_type,
                      float waste_kg, float route_km) {
    int   best       = -1;
    float best_score = FLT_MAX;

    int is_hazardous_req = (waste_type && strcmp(waste_type, "hazardous") == 0);

    for (int i = 0; i < g_vehicle_count; i++) {
        Vehicle *v = &g_vehicles[i];

        /* ── Hard constraints ── */
        if (!v->available || v->assigned_route || v->needs_maintenance) continue;

        /* Hazardous waste → only hazard_capable or hazardous-class vehicles */
        if (is_hazardous_req) {
            int haz_ok = v->hazard_capable ||
                         (strcmp(v->vehicle_class, "hazardous") == 0);
            if (!haz_ok) continue;
        } else {
            /* Non-hazardous waste → do NOT use dedicated hazardous vehicles
               (this is the key fix: V9 is hazardous-class, so it won't
                monopolise general waste routes) */
            if (strcmp(v->vehicle_class, "hazardous") == 0 && !v->hazard_capable)
                continue;
            /* Also skip if flagged hazard_capable but class=hazardous
               (keep hazardous vehicles for hazardous bins) */
            if (strcmp(v->vehicle_class, "hazardous") == 0) continue;
        }

        /* Load capacity check */
        float slack = v->capacity_kg - v->current_load - waste_kg;
        if (slack < 0) continue;

        /* Fuel check: need fuel for estimated route + 25% buffer */
        float fuel_need = route_km * v->fuel_rate * 1.30f;
        if (v->fuel_level < fuel_need) continue;

        /* ── Soft scoring ── */
        float score = 0.0f;

        /* 1. Capacity fit: penalise over-capacity (waste of big trucks on small jobs)
              prefer trucks where capacity is 1.5x–4x the waste load */
        float cap_ratio = v->capacity_kg / (waste_kg > 0 ? waste_kg : 1.0f);
        if      (cap_ratio < 1.5f) score += 80.0f;  /* too small */
        else if (cap_ratio <= 4.0f) score +=  0.0f;  /* ideal fit */
        else if (cap_ratio <= 8.0f) score += 20.0f;  /* slightly oversized */
        else                        score += 50.0f;  /* very oversized */

        /* 2. Zone match: strongly prefer same zone */
        int same_zone = (zone && zone[0] && strcmp(v->zone, zone) == 0);
        if (!same_zone) score += 150.0f;

        /* 3. Fuel adequacy: prefer more fuel headroom (safety) */
        float fuel_headroom = v->fuel_level - fuel_need;
        score += 60.0f * (1.0f - (fuel_headroom / (v->fuel_capacity > 0 ? v->fuel_capacity : 100.0f)));

        /* 4. Load balance: prefer vehicles with less current load (spread work) */
        float load_frac = v->current_load / (v->capacity_kg > 0 ? v->capacity_kg : 1.0f);
        score += 40.0f * load_frac;

        /* 5. Idle fairness: prefer vehicle that has travelled least today */
        float dist_frac = v->total_distance / 200.0f;  /* normalise to ~200km day */
        if (dist_frac > 1.0f) dist_frac = 1.0f;
        score += 25.0f * dist_frac;

        if (score < best_score) { best_score = score; best = i; }
    }
    return best;
}

int vehicle_add(Vehicle *v) {
    if (vehicle_find(v->vehicle_id) >= 0) return -1;
    if (g_vehicle_count >= MAX_VEHICLES) return -2;
    if (v->fuel_rate     <= 0) v->fuel_rate     = 0.12f;
    if (v->fuel_capacity <= 0) v->fuel_capacity = 100.0f;
    if (v->maintenance_due_km <= 0) v->maintenance_due_km = 500;
    if (!v->vehicle_class[0]) strncpy(v->vehicle_class,"normal",15);
    v->current_lat = g_facilities[FACILITY_GARAGE].lat;
    v->current_lon = g_facilities[FACILITY_GARAGE].lon;
    strncpy(v->status,"idle",sizeof(v->status)-1);
    g_vehicles[g_vehicle_count++] = *v;
    storage_save_vehicles();
    sim_log("INFO","VEHICLE","Added vehicle %d (%s) zone=%s", v->vehicle_id, v->plate, v->zone);
    return 0;
}

int vehicle_refuel(int vid, float litres) {
    int i = vehicle_find(vid); if (i<0) return -1;
    g_vehicles[i].fuel_level += litres;
    if (g_vehicles[i].fuel_level > g_vehicles[i].fuel_capacity)
        g_vehicles[i].fuel_level = g_vehicles[i].fuel_capacity;
    g_vehicles[i].needs_maintenance = 0; /* refuel clears service flag */
    storage_save_vehicles();
    sim_log("INFO","VEHICLE","Vehicle %d refuelled +%.1fL → %.1fL", vid, litres, g_vehicles[i].fuel_level);
    return 0;
}

int vehicle_service(int vid) {
    int i = vehicle_find(vid); if (i<0) return -1;
    g_vehicles[i].needs_maintenance = 0;
    g_vehicles[i].total_distance    = 0;  /* reset daily odometer */
    /* lifetime_km NOT reset — that's for records */
    strncpy(g_vehicles[i].status,"idle",sizeof(g_vehicles[i].status)-1);
    g_today.maintenance_events++;
    storage_save_vehicles();
    sim_log("EVENT","VEHICLE",
        "V%d %s serviced — breakdown cleared, daily km reset",
        g_vehicles[i].vehicle_id, g_vehicles[i].plate);
    return 0;
}

int vehicle_delete(int vid) {
    int i = vehicle_find(vid); if (i<0) return -1;
    for (int j=i; j<g_vehicle_count-1; j++) g_vehicles[j]=g_vehicles[j+1];
    g_vehicle_count--;
    storage_save_vehicles();
    return 0;
}

void vehicle_deduct_fuel(int vid, float km) {
    int i = vehicle_find(vid); if (i<0) return;
    Vehicle *v = &g_vehicles[i];
    /* Realistic fuel drain: base rate × 1.8 (city driving, load weight) */
    float used = km * v->fuel_rate * 1.8f;
    v->fuel_level     -= used;
    v->total_distance += km;
    v->lifetime_km    += km;
    if (v->fuel_level < 0) v->fuel_level = 0;

    /* Maintenance trigger: when daily distance exceeds threshold */
    if (v->total_distance >= (float)v->maintenance_due_km)
        v->needs_maintenance = 1;

    /* Random breakdown: 8% chance per route if vehicle is overdue */
    if (!v->needs_maintenance && v->total_distance > (float)v->maintenance_due_km * 0.7f) {
        if ((rand() % 100) < 8) {
            v->needs_maintenance = 1;
            sim_log("WARN","VEHICLE",
                "V%d %s BREAKDOWN at %.1fkm — needs_maintenance set",
                v->vehicle_id, v->plate, v->total_distance);
        }
    }
}

void vehicle_add_load(int vid, float kg) {
    int i = vehicle_find(vid); if (i<0) return;
    g_vehicles[i].current_load += kg;
    strncpy(g_vehicles[i].status,"collecting",sizeof(g_vehicles[i].status)-1);
}

void vehicle_unload(int vid) {
    int i = vehicle_find(vid); if (i<0) return;
    g_vehicles[i].current_load = 0;
    strncpy(g_vehicles[i].status,"idle",sizeof(g_vehicles[i].status)-1);
}

void vehicle_set_status(int vid, const char *s) {
    int i = vehicle_find(vid); if (i<0) return;
    strncpy(g_vehicles[i].status, s, sizeof(g_vehicles[i].status)-1);
}

void vehicle_reset_daily(void) {
    for (int i=0; i<g_vehicle_count; i++) {
        Vehicle *v = &g_vehicles[i];
        v->assigned_route = 0;
        v->current_load   = 0;
        v->total_distance = 0;
        /* Overnight: partial refuel (+30L), NOT full tank — driver must refuel */
        float overnight_add = 30.0f;
        v->fuel_level += overnight_add;
        if (v->fuel_level > v->fuel_capacity) v->fuel_level = v->fuel_capacity;
        strncpy(v->status,"idle",sizeof(v->status)-1);
        v->current_lat = g_facilities[FACILITY_GARAGE].lat;
        v->current_lon = g_facilities[FACILITY_GARAGE].lon;
        /* DO NOT auto-clear needs_maintenance — must be explicitly serviced */
    }
}

char *vehicles_to_json(void) {
    int sz = 64 + g_vehicle_count * 600;
    char *buf = malloc(sz); if (!buf) return NULL;
    int p = snprintf(buf, sz, "[");
    for (int i=0; i<g_vehicle_count; i++) {
        Vehicle *v = &g_vehicles[i];
        float load_pct = v->capacity_kg>0 ? v->current_load/v->capacity_kg*100.0f : 0;
        float fuel_pct = v->fuel_capacity>0 ? v->fuel_level/v->fuel_capacity*100.0f : 0;
        p += snprintf(buf+p, sz-p,
            "%s{"
            "\"vehicle_id\":%d,\"plate\":\"%s\",\"vehicle_class\":\"%s\","
            "\"capacity_kg\":%.0f,\"current_load\":%.1f,\"load_pct\":%.1f,"
            "\"fuel_level\":%.1f,\"fuel_capacity\":%.0f,\"fuel_pct\":%.1f,"
            "\"fuel_rate\":%.3f,\"zone\":\"%s\","
            "\"available\":%s,\"assigned_route\":%d,\"driver_id\":%d,"
            "\"total_distance\":%.1f,\"lifetime_km\":%.0f,"
            "\"needs_maintenance\":%s,\"maintenance_due_km\":%d,"
            "\"status\":\"%s\","
            "\"co2_kg\":%.2f,\"hazard_capable\":%s"
            "}",
            i?",":"",
            v->vehicle_id, v->plate, v->vehicle_class,
            v->capacity_kg, v->current_load, load_pct,
            v->fuel_level, v->fuel_capacity, fuel_pct,
            v->fuel_rate, v->zone,
            v->available?"true":"false", v->assigned_route, v->driver_id,
            v->total_distance, v->lifetime_km,
            v->needs_maintenance?"true":"false", v->maintenance_due_km,
            v->status,
            v->co2_kg, v->hazard_capable?"true":"false");
        if (p > sz-400) break;
    }
    snprintf(buf+p, sz-p, "]"); return buf;
}
