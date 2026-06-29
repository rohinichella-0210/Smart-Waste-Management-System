#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <float.h>
#include <time.h>
#include "types.h"
#include "routes.h"
#include "bins.h"
#include "vehicles.h"
#include "drivers.h"
#include "storage.h"
#include "simulation.h"

Route g_routes[MAX_ROUTES];
int   g_route_count = 0;

/* ── Haversine ─────────────────────────────────────────────── */
float route_haversine(float lat1, float lon1, float lat2, float lon2) {
    const float R = 6371.0f;
    float dlat = (lat2-lat1)*(float)M_PI/180.0f;
    float dlon = (lon2-lon1)*(float)M_PI/180.0f;
    float a = sinf(dlat/2)*sinf(dlat/2)+
              cosf(lat1*(float)M_PI/180.0f)*cosf(lat2*(float)M_PI/180.0f)*
              sinf(dlon/2)*sinf(dlon/2);
    return R*2.0f*atan2f(sqrtf(a),sqrtf(1-a));
}

/* ── Find nearest disposal facility for waste_type ─────────── */
int route_nearest_disposal(const char *wt, float from_lat, float from_lon) {
    int target_type = FACILITY_DISPOSAL;
    if (wt && strcmp(wt,"recycling")==0) target_type = FACILITY_RECYCLE;
    if (wt && strcmp(wt,"hazardous")==0) target_type = FACILITY_HAZARD;

    int best=-1; float bd=FLT_MAX;
    for (int i=0; i<NUM_FACILITIES; i++) {
        if (g_facilities[i].type != target_type) continue;
        float d = route_haversine(from_lat, from_lon,
                                  g_facilities[i].lat, g_facilities[i].lon);
        if (d<bd){ bd=d; best=i; }
    }
    /* Fallback to any disposal */
    if (best<0) {
        for (int i=0; i<NUM_FACILITIES; i++){
            if (g_facilities[i].type==FACILITY_DISPOSAL){best=i;break;}
        }
    }
    return best;
}

/* ── Distance matrix with node 0=garage, 1..n=bins ─────────── */
static void build_matrix(int *bi, int n, float m[][MAX_BINS_PER_RT+1],
                         float glat, float glon) {
    for (int i=0; i<=n; i++) for (int j=0; j<=n; j++) {
        if (i==j){m[i][j]=0;continue;}
        float la_i = (i==0)?glat:g_bins[bi[i-1]].lat;
        float lo_i = (i==0)?glon:g_bins[bi[i-1]].lon;
        float la_j = (j==0)?glat:g_bins[bi[j-1]].lat;
        float lo_j = (j==0)?glon:g_bins[bi[j-1]].lon;
        m[i][j] = route_haversine(la_i,lo_i,la_j,lo_j);
    }
}

/* ── Greedy NN ──────────────────────────────────────────────── */
static void greedy_nn(int n, float m[][MAX_BINS_PER_RT+1], int *order) {
    int vis[MAX_BINS_PER_RT+1]={0};
    int step=0, cur=0;
    order[step++]=0; vis[0]=1;
    for (int k=0;k<n;k++){
        float bd=FLT_MAX; int bn=-1;
        for (int j=1;j<=n;j++) if(!vis[j]&&m[cur][j]<bd){bd=m[cur][j];bn=j;}
        if (bn<0) break;
        vis[bn]=1; order[step++]=bn; cur=bn;
    }
    order[step]=0;
}

/* ── 2-opt ──────────────────────────────────────────────────── */
static float seg_dist(int *order, int steps, float m[][MAX_BINS_PER_RT+1]) {
    float t=0; for(int i=0;i<steps;i++) t+=m[order[i]][order[i+1]]; return t;
}
static void two_opt(int *order, int n, float m[][MAX_BINS_PER_RT+1]) {
    int imp=1;
    while(imp){ imp=0;
        for(int i=1;i<=n-1;i++) for(int j=i+1;j<=n;j++){
            float db=m[order[i-1]][order[i]]+m[order[j]][order[j+1]];
            float da=m[order[i-1]][order[j]]+m[order[i]][order[j+1]];
            if(da<db-1e-5f){
                int lo=i,hi=j;
                while(lo<hi){int t=order[lo];order[lo]=order[hi];order[hi]=t;lo++;hi--;}
                imp=1;
            }
        }
    }
}

/* ── Build human-readable route string ─────────────────────── */
static void build_route_str(Route *r, int *order, int n, int *bi) {
    /* Garage → [bins] → DisposalSite → Garage */
    int p=snprintf(r->route_string,sizeof(r->route_string),"Garage");
    for(int i=1;i<=n;i++){
        int nd=order[i]; if(nd==0)break;
        int idx=bi[nd-1];
        p+=snprintf(r->route_string+p,sizeof(r->route_string)-p,
            " → Bin%d[%s]", g_bins[idx].bin_id, g_bins[idx].location);
        if(p>900)break;
    }
    p+=snprintf(r->route_string+p,sizeof(r->route_string)-p,
        " → %s → Garage", r->disposal_name);
}

/* ── Core: create one route for a batch of bins with one vehicle ─
   Handles mid-trip disposal if load exceeds capacity mid-route.
   Returns route_id or -1 on failure.
 ─────────────────────────────────────────────────────────────── */
static int commit_route(int vi, int *batch_bins, int nb,
                        int *order, float fuel_price, int is_emg) {
    if (g_route_count >= MAX_ROUTES) return -1;
    Vehicle *v = &g_vehicles[vi];

    /* Determine disposal site (use majority waste type) */
    int gen=0,rec=0,haz=0,org=0,dry_c=0,wet_c=0,mix_c=0;
    for(int i=0;i<nb;i++){
        const char *wt=g_bins[batch_bins[i]].waste_type;
        if(!strcmp(wt,"recycling")||!strcmp(wt,"dry")) { rec++; dry_c++; }
        else if(!strcmp(wt,"hazardous")) haz++;
        else if(!strcmp(wt,"organic")||!strcmp(wt,"wet")) { org++; wet_c++; }
        else if(!strcmp(wt,"mixed")) mix_c++;
        else gen++;
    }
    (void)dry_c;(void)wet_c;(void)mix_c; /* suppress unused warnings */
    const char *dom_wt = (haz>=gen&&haz>=rec&&haz>=org)?"hazardous":
                         (rec>=gen&&rec>=org)?"recycling":"general";

    /* Last bin coords → nearest disposal */
    int last_bi = batch_bins[order[nb]-1];
    int dfi = route_nearest_disposal(dom_wt, g_bins[last_bi].lat, g_bins[last_bi].lon);
    if (dfi<0) dfi=1; /* fallback */

    Facility *disp = &g_facilities[dfi];
    float glat = g_facilities[FACILITY_GARAGE].lat;
    float glon = g_facilities[FACILITY_GARAGE].lon;

    /* Compute distance matrix */
    float mat[MAX_BINS_PER_RT+1][MAX_BINS_PER_RT+1];
    build_matrix(batch_bins, nb, mat, glat, glon);

    float dist_bins = seg_dist(order, nb, mat); /* garage→bins total */
    float d_garage_first = mat[0][order[1]];
    float d_last_disp = route_haversine(g_bins[last_bi].lat, g_bins[last_bi].lon,
                                         disp->lat, disp->lon);
    float d_disp_garage = route_haversine(disp->lat, disp->lon, glat, glon);

    /* Check for mid-trip disposal (if load would exceed capacity at some point) */
    int mid_disposal = 0;
    float running_load = 0;
    float extra_dist = 0; /* extra km for mid-trip disposal detours */
    for (int i=1; i<=nb; i++) {
        int nd = order[i]; if(nd==0) break;
        Bin *b = &g_bins[batch_bins[nd-1]];
        float w = b->fill_level * b->capacity_kg / 100.0f;
        if (running_load + w > v->capacity_kg && running_load > 0) {
            /* Need to detour to disposal mid-route */
            mid_disposal = 1;
            /* Add disposal trip from current bin to disp and back */
            float cur_lat = g_bins[batch_bins[order[i-1]-1]].lat;
            float cur_lon = g_bins[batch_bins[order[i-1]-1]].lon;
            int mid_dfi = route_nearest_disposal(dom_wt, cur_lat, cur_lon);
            if (mid_dfi<0) mid_dfi=1;
            Facility *md = &g_facilities[mid_dfi];
            extra_dist += route_haversine(cur_lat,cur_lon,md->lat,md->lon) * 2.0f;
            running_load = 0; /* unloaded */
        }
        running_load += w;
    }

    float total_dist = d_garage_first + dist_bins + d_last_disp
                     + d_disp_garage + extra_dist;
    float fuel_req   = total_dist * v->fuel_rate;

    /* Fuel check */
    if (fuel_req > v->fuel_level) return -1;

    Route *rt = &g_routes[g_route_count];
    memset(rt, 0, sizeof(Route));
    rt->route_id             = g_route_count + 1;
    rt->vehicle_id           = v->vehicle_id;
    rt->bin_count            = nb;
    rt->dist_garage_to_first = d_garage_first;
    rt->dist_bins            = dist_bins + extra_dist;
    rt->dist_to_disposal     = d_last_disp;
    rt->dist_disposal_to_garage = d_disp_garage;
    rt->total_distance       = total_dist;
    rt->fuel_required        = fuel_req;
    rt->fuel_cost            = fuel_req * fuel_price;
    rt->is_emergency         = is_emg;
    rt->mid_trip_disposal    = mid_disposal;
    rt->disposal_facility_id = dfi;
    strncpy(rt->disposal_name, disp->name, sizeof(rt->disposal_name)-1);
    strncpy(rt->zone, "multi", sizeof(rt->zone)-1);

    /* Fill bin_sequence in optimized visit order */
    for (int i=1; i<=nb; i++) {
        int nd=order[i]; if(nd==0||nd>nb) break;
        rt->bin_sequence[i-1] = g_bins[batch_bins[nd-1]].bin_id;
    }
    build_route_str(rt, order, nb, batch_bins);

    time_t now=time(NULL);
    strftime(rt->created_at,sizeof(rt->created_at),"%Y-%m-%d %H:%M:%S",localtime(&now));

    v->assigned_route = rt->route_id;
    g_route_count++;
    return rt->route_id;
}

/* ── Auto-Schedule ─────────────────────────────────────────────
   Strategy:
   1. Separate hazardous bins → only hazardous-capable vehicles
   2. Group remaining bins BY ZONE → one vehicle per zone
   3. Each zone uses its own best-matched vehicle simultaneously
   4. Hazardous bins always routed to FACILITY_HAZARD
   5. Uses current-shift drivers only
   ──────────────────────────────────────────────────────────── */
ScheduleResult routes_auto_schedule(float fuel_price) {
    ScheduleResult res = {0};
    float glat = g_facilities[FACILITY_GARAGE].lat;
    float glon = g_facilities[FACILITY_GARAGE].lon;

    /* 1. Collect all uncollected bins at >=70% fill */
    int crit[MAX_BINS]; int nc = 0;
    for (int i = 0; i < g_bin_count; i++) {
        if (g_bins[i].collected_today) continue;
        if (g_bins[i].fill_level >= 70.0f || g_bins[i].priority >= 1) crit[nc++] = i;
    }
    if (nc == 0) {
        snprintf(res.message, sizeof(res.message), "No critical bins.");
        res.success = 1; return res;
    }

    /* 2. Sort by priority_order (computed by bins_assign_priority_orders) */
    /* Recompute first to ensure fresh ordering */
    bins_assign_priority_orders();
    for (int i = 0; i < nc-1; i++) for (int j = i+1; j < nc; j++) {
        int oi = g_bins[crit[i]].priority_order;
        int oj = g_bins[crit[j]].priority_order;
        if (oi == 0) oi = 9999; /* unranked goes last */
        if (oj == 0) oj = 9999;
        if (oj < oi) { int t = crit[i]; crit[i] = crit[j]; crit[j] = t; }
    }

    int assigned[MAX_BINS] = {0};

    /* ── PASS A: Hazardous bins — dedicated small-batch routes ── */
    for (int i = 0; i < nc; i++) {
        int bi = crit[i];
        if (assigned[bi]) continue;
        if (strcmp(g_bins[bi].waste_type, "hazardous") != 0) continue;

        /* Find hazard-capable vehicle */
        float w = g_bins[bi].fill_level * g_bins[bi].capacity_kg / 100.0f;
        int vi = vehicle_find_best(g_bins[bi].zone, "hazardous", w, 20.0f);
        if (vi < 0) {
            sim_log("WARN","ROUTE","No hazardous vehicle for Bin%d [%s]",
                g_bins[bi].bin_id, g_bins[bi].location);
            continue;
        }
        Vehicle *v = &g_vehicles[vi];

        /* Batch all hazardous bins this vehicle can carry */
        int batch[MAX_BINS_PER_RT]; int nb = 0; float load = 0;
        for (int j = i; j < nc && nb < MAX_BINS_PER_RT; j++) {
            int bj = crit[j];
            if (assigned[bj]) continue;
            if (strcmp(g_bins[bj].waste_type, "hazardous") != 0) continue;
            float wj = g_bins[bj].fill_level * g_bins[bj].capacity_kg / 100.0f;
            if (load + wj > v->capacity_kg && nb > 0) break; /* capacity full */
            batch[nb++] = bj; load += wj;
            if (nb >= 12) break; /* max 12 hazardous stops per route */
        }
        if (nb == 0) continue;

        float mat[MAX_BINS_PER_RT+1][MAX_BINS_PER_RT+1];
        build_matrix(batch, nb, mat, glat, glon);
        int order[MAX_BINS_PER_RT+2] = {0};
        greedy_nn(nb, mat, order);
        two_opt(order, nb, mat);

        int di = driver_find_for_vehicle(v->vehicle_id);
        if (di < 0) di = driver_find_substitute(NULL);
        float est_km  = seg_dist(order, nb, mat) + 10.0f;
        float est_hrs = est_km / 35.0f + nb * 0.15f; /* hazard handling slower */

        int rid = commit_route(vi, batch, nb, order, fuel_price, 0);
        if (rid < 0) continue;

        Route *rt = &g_routes[g_route_count-1];
        rt->has_hazardous = 1;
        for (int j = 0; j < nb; j++) assigned[batch[j]] = 1;
        if (di >= 0) {
            driver_log_hours(g_drivers[di].driver_id, est_hrs);
            rt->labor_cost = est_hrs * g_drivers[di].salary_per_hour;
            rt->total_cost = rt->fuel_cost + rt->labor_cost;
        }
        res.routes_created++;
        res.bins_scheduled += nb;
        res.total_distance += rt->total_distance;
        res.total_fuel     += rt->fuel_required;
        res.total_cost     += rt->total_cost;

        sim_log("EVENT","ROUTE",
            "Hazardous route #%d: V%d → %d bins → %s",
            rid, v->vehicle_id, nb, rt->disposal_name);
        break; /* one hazardous route per schedule call; rest next sim */
    }

    /* ── PASS B: Normal bins grouped BY ZONE ── */
    /* Collect unique zones */
    char zones[32][16]; int nz = 0;
    for (int i = 0; i < nc; i++) {
        if (assigned[crit[i]]) continue;
        const char *z = g_bins[crit[i]].zone;
        int found = 0;
        for (int j = 0; j < nz; j++) if (!strcmp(zones[j], z)) { found=1; break; }
        if (!found && nz < 32) strncpy(zones[nz++], z, 15);
    }

    /* One vehicle per zone */
    for (int zi = 0; zi < nz && res.routes_created < 20; zi++) {
        /* Collect bins in this zone */
        int batch[MAX_BINS_PER_RT]; int nb = 0;
        for (int i = 0; i < nc && nb < MAX_BINS_PER_RT; i++) {
            int bi = crit[i];
            if (assigned[bi]) continue;
            if (strcmp(g_bins[bi].zone, zones[zi]) != 0) continue;
            if (!strcmp(g_bins[bi].waste_type, "hazardous")) continue;
            batch[nb++] = bi;
        }
        if (nb == 0) continue;

        /* Estimate total waste for this zone */
        float zone_waste = 0;
        for (int i = 0; i < nb; i++)
            zone_waste += g_bins[batch[i]].fill_level * g_bins[batch[i]].capacity_kg / 100.0f;

        float rough_km = 3.0f + nb * 1.8f;

        /* Find best vehicle for this zone */
        int vi = vehicle_find_best(zones[zi], "general", zone_waste, rough_km);
        if (vi < 0) {
            /* Try any zone */
            vi = vehicle_find_best("", "general", zone_waste, rough_km);
        }
        if (vi < 0) {
            sim_log("WARN","ROUTE","No vehicle available for zone %s (%d bins)", zones[zi], nb);
            continue;
        }
        Vehicle *v = &g_vehicles[vi];

        /* If zone has more bins than vehicle can carry in one load,
           cap the batch — remaining bins will be picked in next sim */
        float cap_load = 0; int capped_nb = 0;
        for (int i = 0; i < nb; i++) {
            float w = g_bins[batch[i]].fill_level * g_bins[batch[i]].capacity_kg / 100.0f;
            cap_load += w;
            capped_nb++;
            if (capped_nb >= 20) break; /* max 20 bins per route */
        }
        nb = capped_nb;

        float mat[MAX_BINS_PER_RT+1][MAX_BINS_PER_RT+1];
        build_matrix(batch, nb, mat, glat, glon);
        int order[MAX_BINS_PER_RT+2] = {0};
        greedy_nn(nb, mat, order);
        two_opt(order, nb, mat);

        int di = driver_find_for_vehicle(v->vehicle_id);
        if (di < 0) di = driver_find_substitute(NULL);

        float est_km  = seg_dist(order, nb, mat) + 10.0f;
        float est_hrs = est_km / 35.0f + nb * 0.1f;

        /* Skip if driver is exhausted (no overtime beyond 2h) */
        if (di >= 0 && g_drivers[di].hours_worked + est_hrs > g_drivers[di].max_hours + 2.0f) {
            /* Try substitute */
            int si = driver_find_substitute(NULL);
            if (si >= 0) di = si;
            else {
                sim_log("WARN","ROUTE","Zone %s: driver exhausted, no sub available", zones[zi]);
                continue;
            }
        }

        int rid = commit_route(vi, batch, nb, order, fuel_price, 0);
        if (rid < 0) continue;

        Route *rt = &g_routes[g_route_count-1];
        for (int i = 0; i < nb; i++) assigned[batch[i]] = 1;
        if (di >= 0) {
            driver_log_hours(g_drivers[di].driver_id, est_hrs);
            rt->labor_cost = est_hrs * g_drivers[di].salary_per_hour;
            rt->total_cost = rt->fuel_cost + rt->labor_cost;
        }
        res.routes_created++;
        res.bins_scheduled += nb;
        res.total_distance += rt->total_distance;
        res.total_fuel     += rt->fuel_required;
        res.total_cost     += rt->total_cost;

        sim_log("EVENT","ROUTE",
            "Zone route #%d: V%d [%s] → %d bins → %s",
            rid, v->vehicle_id, zones[zi], nb, rt->disposal_name);
    }

    storage_save_routes(); storage_save_vehicles(); storage_save_drivers();
    res.success = 1;
    snprintf(res.message, sizeof(res.message),
        "%d routes, %d bins, %.1fkm, %.2fL, cost=%.2f",
        res.routes_created, res.bins_scheduled,
        res.total_distance, res.total_fuel, res.total_cost);
    return res;
}

/* ── Emergency Route ───────────────────────────────────────── */
EmergencyResult routes_emergency(int bin_id, const char *reason) {
    EmergencyResult er={0};
    int bi=bin_find(bin_id);
    if(bi<0){snprintf(er.message,sizeof(er.message),"Bin %d not found",bin_id);return er;}
    Bin *b=&g_bins[bi];
    b->fill_level=100.0f; b->priority=3;

    /* Clear stale assigned_route where route is already completed */
    for(int i=0;i<g_vehicle_count;i++){
        Vehicle *v=&g_vehicles[i];
        if(!v->assigned_route) continue;
        int ri=route_find(v->assigned_route);
        if(ri>=0 && g_routes[ri].completed) v->assigned_route=0;
    }

    /* Find nearest available suitable vehicle */
    float best_d=FLT_MAX; int best_vi=-1;
    for(int i=0;i<g_vehicle_count;i++){
        Vehicle *v=&g_vehicles[i];
        if(!v->available||v->assigned_route||v->needs_maintenance) continue;
        if(strcmp(b->waste_type,"hazardous")==0&&strcmp(v->vehicle_class,"hazardous")!=0) continue;
        if(v->fuel_level<2.0f) continue;
        float d=(strcmp(v->zone,b->zone)==0)?0.0f:50.0f;
        if(d<best_d){best_d=d;best_vi=i;}
    }
    /* Fallback: all vehicles blocked by a pending emergency route — free the oldest and reuse */
    if(best_vi<0){
        int oldest_route=999999; int fallback_vi=-1;
        for(int i=0;i<g_vehicle_count;i++){
            Vehicle *v=&g_vehicles[i];
            if(!v->available||v->needs_maintenance) continue;
            if(strcmp(b->waste_type,"hazardous")==0&&strcmp(v->vehicle_class,"hazardous")!=0) continue;
            if(v->fuel_level<2.0f) continue;
            if(v->assigned_route && v->assigned_route<oldest_route){
                int ri=route_find(v->assigned_route);
                if(ri>=0 && g_routes[ri].is_emergency && !g_routes[ri].completed){
                    oldest_route=v->assigned_route; fallback_vi=i;
                }
            }
        }
        if(fallback_vi>=0){
            int ri=route_find(g_vehicles[fallback_vi].assigned_route);
            if(ri>=0){g_routes[ri].completed=1;}
            g_vehicles[fallback_vi].assigned_route=0;
            best_vi=fallback_vi;
        }
    }
    if(best_vi<0){snprintf(er.message,sizeof(er.message),"No vehicle available");return er;}

    Vehicle *v=&g_vehicles[best_vi];
    float glat=g_facilities[FACILITY_GARAGE].lat, glon=g_facilities[FACILITY_GARAGE].lon;
    int dfi=route_nearest_disposal(b->waste_type,b->lat,b->lon);
    if(dfi<0)dfi=1;
    Facility *disp=&g_facilities[dfi];

    float d_g2b=route_haversine(glat,glon,b->lat,b->lon);
    float d_b2d=route_haversine(b->lat,b->lon,disp->lat,disp->lon);
    float d_d2g=route_haversine(disp->lat,disp->lon,glat,glon);
    float total=d_g2b+d_b2d+d_d2g;
    float fuel=total*v->fuel_rate;
    if(fuel>v->fuel_level){
        snprintf(er.message,sizeof(er.message),"Vehicle %d low fuel (need %.1fL have %.1fL)",
            v->vehicle_id,fuel,v->fuel_level); return er;
    }
    if(g_route_count>=MAX_ROUTES){snprintf(er.message,sizeof(er.message),"Route table full");return er;}

    /* Estimate waste collected (fill_level % of capacity) */
    float waste_kg = b->fill_level * b->capacity_kg / 100.0f;

    /* Fuel cost at default price */
    float fuel_price = 1.20f;
    float fuel_cost  = fuel * fuel_price;

    /* Labor cost: estimate trip time at 30 km/h average, find driver */
    float est_hrs    = (total / 30.0f);
    float labor_cost = 0.0f;
    int di = driver_find_for_vehicle(v->vehicle_id);
    if (di < 0) di = driver_find_substitute(NULL);
    if (di >= 0) labor_cost = est_hrs * g_drivers[di].salary_per_hour;

    Route *rt=&g_routes[g_route_count];
    memset(rt,0,sizeof(Route));
    rt->route_id=g_route_count+1; rt->vehicle_id=v->vehicle_id;
    rt->bin_sequence[0]=bin_id; rt->bin_count=1;
    rt->dist_garage_to_first=d_g2b; rt->dist_to_disposal=d_b2d;
    rt->dist_disposal_to_garage=d_d2g; rt->total_distance=total;
    rt->fuel_required=fuel; rt->is_emergency=1;
    rt->total_waste_kg=waste_kg;
    rt->fuel_cost=fuel_cost;
    rt->labor_cost=labor_cost;
    rt->total_cost=fuel_cost+labor_cost;
    rt->disposal_facility_id=dfi;
    strncpy(rt->disposal_name,disp->name,sizeof(rt->disposal_name)-1);
    snprintf(rt->route_string,sizeof(rt->route_string),
        "[EMERGENCY] Garage → Bin%d[%s] → %s → Garage",
        bin_id, b->location, disp->name);
    time_t now=time(NULL);
    strftime(rt->created_at,sizeof(rt->created_at),"%Y-%m-%d %H:%M:%S",localtime(&now));
    v->assigned_route=rt->route_id;
    g_route_count++;

    storage_save_routes(); storage_save_vehicles();
    er.success=1; er.route_id=rt->route_id; er.vehicle_id=v->vehicle_id;
    er.distance=total;
    snprintf(er.message,sizeof(er.message),
        "V%d dispatched → Bin%d → %s → Garage (%.1fkm,%.2fL)",
        v->vehicle_id,bin_id,disp->name,total,fuel);
    return er;
}

int route_find(int id){
    for(int i=0;i<g_route_count;i++) if(g_routes[i].route_id==id) return i;
    return -1;
}

void routes_clear_completed(void){
    int j=0;
    for(int i=0;i<g_route_count;i++) if(!g_routes[i].completed) g_routes[j++]=g_routes[i];
    g_route_count=j; storage_save_routes();
}

void routes_reset_daily(void){routes_clear_completed();}

char *routes_to_json(void){
    int sz=128+g_route_count*1200;
    char *buf=malloc(sz); if(!buf)return NULL;
    int p=snprintf(buf,sz,"[");
    for(int i=0;i<g_route_count;i++){
        Route *r=&g_routes[i];
        char barr[256]="["; int bp=1;
        for(int j=0;j<r->bin_count;j++)
            bp+=snprintf(barr+bp,sizeof(barr)-bp,"%s%d",j?",":"",r->bin_sequence[j]);
        snprintf(barr+bp,sizeof(barr)-bp,"]");
        /* Escape route_string quotes */
        char rs[1100]={0}; int rp=0;
        for(int c=0;r->route_string[c]&&rp<1090;c++){
            if(r->route_string[c]=='"'){rs[rp++]='\\';rs[rp++]='"';}
            else rs[rp++]=r->route_string[c];
        }
        p+=snprintf(buf+p,sz-p,
            "%s{\"route_id\":%d,\"vehicle_id\":%d,\"zone\":\"%s\","
            "\"bin_count\":%d,\"bins\":%s,"
            "\"dist_garage_first\":%.2f,\"dist_bins\":%.2f,"
            "\"dist_to_disposal\":%.2f,\"dist_disposal_garage\":%.2f,"
            "\"total_distance\":%.2f,\"fuel_required\":%.2f,"
            "\"total_waste_kg\":%.1f,\"fuel_cost\":%.2f,"
            "\"labor_cost\":%.2f,\"total_cost\":%.2f,"
            "\"disposal_site\":\"%s\","
            "\"completed\":%s,\"emergency\":%s,\"mid_disposal\":%s,"
            "\"route_string\":\"%s\","
            "\"created_at\":\"%s\",\"completed_at\":\"%s\"}",
            i?",":"",
            r->route_id,r->vehicle_id,r->zone,
            r->bin_count,barr,
            r->dist_garage_to_first,r->dist_bins,
            r->dist_to_disposal,r->dist_disposal_to_garage,
            r->total_distance,r->fuel_required,
            r->total_waste_kg,r->fuel_cost,
            r->labor_cost,r->total_cost,
            r->disposal_name,
            r->completed?"true":"false",
            r->is_emergency?"true":"false",
            r->mid_trip_disposal?"true":"false",
            rs, r->created_at, r->completed_at);
        if(p>sz-800) break;
    }
    snprintf(buf+p,sz-p,"]"); return buf;
}
