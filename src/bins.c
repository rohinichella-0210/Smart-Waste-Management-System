#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "types.h"
#include "bins.h"
#include "storage.h"
#include "simulation.h"

/* Valid waste types */
static const char *VALID_WASTE_TYPES[] = {"dry","wet","hazardous","mixed",NULL};

int waste_type_valid(const char *wt) {
    if (!wt || !wt[0]) return 0;
    for (int i = 0; VALID_WASTE_TYPES[i]; i++)
        if (strcmp(wt, VALID_WASTE_TYPES[i]) == 0) return 1;
    return 0;
}

Bin  g_bins[MAX_BINS];
int  g_bin_count = 0;

int bins_init(void) { return 0; }

int bin_find(int id) {
    for (int i = 0; i < g_bin_count; i++)
        if (g_bins[i].bin_id == id) return i;
    return -1;
}

void bin_update_priority(int idx) {
    Bin *b = &g_bins[idx];
    if      (b->fill_level >= 100.0f)              b->priority = 3;
    else if (b->fill_level >= 90.0f)               b->priority = 2;
    else if (b->fill_level >= (float)b->threshold) b->priority = 1;
    else                                            b->priority = 0;

    b->predicted_fill = b->fill_level + b->fill_rate;
    if (b->predicted_fill > 100.0f) b->predicted_fill = 100.0f;
}

void bins_update_all_priorities(void) {
    for (int i = 0; i < g_bin_count; i++) bin_update_priority(i);
    bins_assign_priority_orders();
}

void bins_assign_priority_orders(void) {
    /* Build a sorted index array by priority desc, fill_level desc */
    int idx[MAX_BINS];
    for (int i = 0; i < g_bin_count; i++) idx[i] = i;
    /* Bubble sort (small n) */
    for (int i = 0; i < g_bin_count-1; i++) {
        for (int j = i+1; j < g_bin_count; j++) {
            int pi = g_bins[idx[i]].priority, pj = g_bins[idx[j]].priority;
            float fi = g_bins[idx[i]].fill_level, fj = g_bins[idx[j]].fill_level;
            if (pj > pi || (pj == pi && fj > fi)) {
                int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }
        }
    }
    for (int r = 0; r < g_bin_count; r++)
        g_bins[idx[r]].priority_order = r + 1; /* 1-based rank */
}

int bin_add(Bin *b) {
    if (bin_find(b->bin_id) >= 0) return -1;
    if (g_bin_count >= MAX_BINS)  return -2;
    if (b->bin_id <= 0) return -4;           /* empty/invalid bin ID */
    if (!b->location[0]) return -5;
    /* Validate / default waste type */
    if (!waste_type_valid(b->waste_type))
        strncpy(b->waste_type, "mixed", 15);
    /* Validate fill level 0-100 */
    if (b->fill_level < 0) b->fill_level = 0;
    if (b->fill_level > 100) b->fill_level = 100;
    /* Validate capacity */
    if (b->capacity_kg < 0) return -6;       /* negative capacity */
    if (b->lat == 0.0f) b->lat = 13.0827f + b->bin_id * 0.002f;
    if (b->lon == 0.0f) b->lon = 80.2707f + b->bin_id * 0.002f;
    if (b->capacity_kg <= 0) b->capacity_kg = 500.0f;
    if (b->fill_rate   <= 0) b->fill_rate   = 8.0f;
    if (b->threshold   <= 0) b->threshold   = 75;
    g_bins[g_bin_count] = *b;
    bin_update_priority(g_bin_count);
    g_bin_count++;
    bins_assign_priority_orders();
    storage_save_bins();
    sim_log("INFO","BIN","Added bin %d at %s (type=%s)", b->bin_id, b->location, b->waste_type);
    return 0;
}

/* Add kg of waste to a bin — real-time update */
int bin_update_fill(int bin_id, float delta_kg) {
    int i = bin_find(bin_id);
    if (i < 0) return -1;
    Bin *b = &g_bins[i];
    float new_fill = b->fill_level + (delta_kg / b->capacity_kg * 100.0f);
    if (new_fill > 100.0f) new_fill = 100.0f;
    b->fill_level = new_fill;
    bin_update_priority(i);
    bins_assign_priority_orders();
    storage_save_bins();
    sim_log("INFO","BIN","Bin %d fill updated +%.1fkg → %.1f%%", bin_id, delta_kg, new_fill);
    return 0;
}

int bin_set_fill(int bin_id, float new_pct) {
    int i = bin_find(bin_id);
    if (i < 0) return -1;
    if (new_pct < 0) new_pct = 0;
    if (new_pct > 100) new_pct = 100;
    g_bins[i].fill_level = new_pct;
    bin_update_priority(i);
    storage_save_bins();
    return 0;
}

int bin_delete(int bin_id) {
    int i = bin_find(bin_id);
    if (i < 0) return -1;
    for (int j = i; j < g_bin_count-1; j++) g_bins[j] = g_bins[j+1];
    g_bin_count--;
    storage_save_bins();
    return 0;
}

/* Called each new day — simulate overnight fill accumulation */
void bins_simulate_daily_fill(void) {
    for (int i = 0; i < g_bin_count; i++) {
        Bin *b = &g_bins[i];
        float noise = ((float)(rand() % 11) - 5.0f) * 0.5f;
        b->fill_level += b->fill_rate + noise;
        if (b->fill_level > 100.0f) b->fill_level = 100.0f;
        if (b->fill_level < 0.0f)   b->fill_level = 0.0f;
        bin_update_priority(i);
    }
}

void bins_reset_collected(void) {
    for (int i = 0; i < g_bin_count; i++)
        g_bins[i].collected_today = 0;
}

char *bins_to_json(void) {
    int sz = 64 + g_bin_count * 600;
    char *buf = malloc(sz); if (!buf) return NULL;
    int p = snprintf(buf, sz, "[");
    const char *pn[] = {"normal","high","critical","overflow"};
    for (int i = 0; i < g_bin_count; i++) {
        Bin *b = &g_bins[i];
        int pr = b->priority; if (pr<0||pr>3) pr=0;
        p += snprintf(buf+p, sz-p,
            "%s{\"bin_id\":%d,\"location\":\"%s\",\"fill_level\":%.1f,"
            "\"capacity_kg\":%.0f,\"waste_type\":\"%s\",\"threshold\":%d,"
            "\"zone\":\"%s\",\"priority\":%d,\"priority_name\":\"%s\","
            "\"priority_order\":%d,"
            "\"predicted_fill\":%.1f,\"fill_rate\":%.1f,"
            "\"lat\":%.6f,\"lon\":%.6f,"
            "\"collected_today\":%s,\"last_collected\":\"%s\","
            "\"total_waste_collected\":%.1f}",
            i?",":"", b->bin_id, b->location, b->fill_level,
            b->capacity_kg, b->waste_type, b->threshold,
            b->zone, pr, pn[pr],
            b->priority_order,
            b->predicted_fill, b->fill_rate,
            b->lat, b->lon,
            b->collected_today?"true":"false", b->last_collected,
            b->total_waste_collected);
        if (p > sz-200) break;
    }
    snprintf(buf+p, sz-p, "]");
    return buf;
}
