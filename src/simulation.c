#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "types.h"
#include "simulation.h"
#include "bins.h"
#include "vehicles.h"
#include "drivers.h"
#include "routes.h"
#include "storage.h"
#include "heatmap.h"

/* ── Global state ─────────────────────────────────────────── */
TruckStep g_sim_steps[MAX_TRUCK_STEPS];
int       g_sim_step_count = 0;
Facility g_facilities[NUM_FACILITIES] = {
    {0,"Vyasarpadi Garage",    FACILITY_GARAGE,  13.1100f, 80.2600f, 0,    0},
    {1,"Kodungaiyur Dumping",  FACILITY_DISPOSAL,13.1306f, 80.2814f, 50000,0},
    {2,"Perungudi Recycling",  FACILITY_RECYCLE, 12.9756f, 80.2479f, 20000,0},
    {3,"Manali Hazardous Site",FACILITY_HAZARD,  13.1696f, 80.2614f, 5000, 0},
};

LogEntry  g_log[MAX_LOG_ENTRIES];
int       g_log_count = 0;
DailyStats g_today    = {0};
DailyStats g_history[365];
int        g_history_count = 0;

static int  g_emergency_alerts = 0;
static int  g_sim_count        = 0; /* simulations run this session */
static char g_last_day[20]     = "";

/* ── Logging ───────────────────────────────────────────────── */
void sim_log(const char *level, const char *module, const char *fmt, ...) {
    if (g_log_count >= MAX_LOG_ENTRIES) {
        /* Ring buffer: shift */
        memmove(g_log, g_log+1, (MAX_LOG_ENTRIES-1)*sizeof(LogEntry));
        g_log_count = MAX_LOG_ENTRIES-1;
    }
    LogEntry *e = &g_log[g_log_count++];
    time_t now=time(NULL);
    strftime(e->timestamp,sizeof(e->timestamp),"%Y-%m-%d %H:%M:%S",localtime(&now));
    strncpy(e->level,  level,  sizeof(e->level)-1);
    strncpy(e->module, module, sizeof(e->module)-1);
    va_list ap; va_start(ap,fmt);
    vsnprintf(e->message,sizeof(e->message),fmt,ap);
    va_end(ap);
}

/* ── New Day Reset (NEVER makes simulation exhausted) ───────── */
void sim_new_day(void) {
    time_t now=time(NULL);
    char today[20];
    strftime(today,sizeof(today),"%Y-%m-%d",localtime(&now));

    /* Archive today's stats */
    if (g_history_count < 365)
        g_history[g_history_count++] = g_today;

    /* Reset daily stats */
    memset(&g_today,0,sizeof(g_today));
    strncpy(g_today.date, today, sizeof(g_today.date)-1);
    strncpy(g_last_day, today, sizeof(g_last_day)-1);
    g_emergency_alerts = 0;
    g_sim_count = 0;

    /* Bins: overnight fill */
    bins_simulate_daily_fill();
    bins_reset_collected();

    /* Vehicles: refuel to 80%, clear assignments */
    vehicle_reset_daily();

    /* Drivers: rest, reset hours, redistribute shifts */
    driver_reset_daily();
    drivers_assign_shifts();

    /* Routes: clear completed from previous day */
    routes_clear_completed();

    storage_save_all();
    sim_log("EVENT","SIM","=== NEW DAY %s === Bins refilled, vehicles refuelled, drivers rested",today);
}

/* ── Add waste to bin in real-time ─────────────────────────── */
int sim_add_waste(int bin_id, float kg) {
    int r = bin_update_fill(bin_id, kg);
    if (r==0) {
        int idx = bin_find(bin_id);
        if(idx>=0 && g_bins[idx].priority==3)
            sim_log("WARN","BIN","Bin %d OVERFLOW after adding %.1fkg!", bin_id, kg);
    }
    return r;
}

/* ── Full Simulation — NEVER EXHAUSTS ───────────────────────
   If all bins are collected → auto new-day → simulate again.
   Vehicles that run out of fuel mid-route → refuel + continue.
   Vehicles needing maintenance → mark, skip to next available.
 ─────────────────────────────────────────────────────────────── */
SimResult sim_run(float fuel_price) {
    SimResult sr={0};
    int log_p=0; char simlog[8192]="";

    time_t now_t=time(NULL);
    char ts[20]; strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",localtime(&now_t));
    char today[20]; strftime(today,sizeof(today),"%Y-%m-%d",localtime(&now_t));

    /* Auto new-day if date changed */
    if (g_last_day[0] && strcmp(g_last_day,today)!=0) {
        sim_new_day();
        log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
            "[AUTO-RESET] New calendar day detected — daily reset applied\n");
    }
    if (!g_last_day[0]) strncpy(g_last_day,today,sizeof(g_last_day)-1);

    /* Threshold guard: refuse if no bins at >=70% fill */
    int near_threshold_count = 0;
    int critical_count = 0;
    float max_fill = 0.0f;
    for (int i = 0; i < g_bin_count; i++) {
        if (g_bins[i].collected_today) continue;
        if (g_bins[i].fill_level > max_fill) max_fill = g_bins[i].fill_level;
        if (g_bins[i].fill_level >= 70.0f)   near_threshold_count++;
        if (g_bins[i].priority >= 1)          critical_count++;
    }
    if (near_threshold_count == 0) {
        sr.success = 0;
        snprintf(sr.message, sizeof(sr.message),
            "Threshold not reached — no bins at >=70%% fill (highest: %.1f%%). "
            "Add waste or apply New Day reset.", max_fill);
        sim_log("WARN","SIM",
            "Simulation blocked: no bins at >=70%% fill (max=%.1f%%)", max_fill);
        return sr;
    }
    log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
        "[%s] Simulation #%d | Near-threshold: %d | Critical: %d\n",
        ts, g_sim_count+1, near_threshold_count, critical_count);

    /* Handle maintenance vehicles BEFORE scheduling */
    for(int i=0;i<g_vehicle_count;i++){
        Vehicle *v=&g_vehicles[i];
        if(v->needs_maintenance){
            log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
                "  [MAINTENANCE] V%d %s sent to service — refuelling after\n",
                v->vehicle_id, v->plate);
            vehicle_service(v->vehicle_id);
            vehicle_refuel(v->vehicle_id, v->fuel_capacity);
            g_today.maintenance_events++;
        }
        /* Refuel vehicles below 20% before scheduling */
        if(v->fuel_level < v->fuel_capacity*0.2f && !v->needs_maintenance){
            float add=v->fuel_capacity*0.8f-v->fuel_level;
            vehicle_refuel(v->vehicle_id, add);
            log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
                "  [REFUEL] V%d refuelled +%.1fL before route\n",v->vehicle_id,add);
        }
    }

    /* Auto-schedule all routes */
    ScheduleResult sched = routes_auto_schedule(fuel_price);
    log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
        "  Scheduled: %s\n", sched.message);

    if(!sched.success || sched.routes_created==0){
        sr.success=1;
        snprintf(sr.message,sizeof(sr.message),
            "No routes created: %s",sched.message);
        sim_log("WARN","SIM","No routes created: %s",sched.message);
        return sr;
    }

    /* Reset animated steps */
    g_sim_step_count = 0;

    /* ── Simulate all routes simultaneously ── */
    now_t=time(NULL);
    for(int ri=0;ri<g_route_count;ri++){
        Route *rt=&g_routes[ri]; if(rt->completed) continue;
        int vi=vehicle_find(rt->vehicle_id); if(vi<0) continue;
        Vehicle *v=&g_vehicles[vi];
        int di=driver_find_for_vehicle(v->vehicle_id);
        if(di<0) di=driver_find_substitute(NULL);

        int   collected=0;
        float waste_loaded=0, recycling=0, hazardous=0, general=0, organic=0;
        float dry=0, wet=0, mixed=0;
        int   disposal_trips=1; /* always one final trip */

        /* Record animation: truck starts at garage */
        if (g_sim_step_count < MAX_TRUCK_STEPS) {
            TruckStep *ts = &g_sim_steps[g_sim_step_count++];
            memset(ts, 0, sizeof(*ts));
            ts->route_id    = rt->route_id;
            ts->vehicle_id  = v->vehicle_id;
            strncpy(ts->plate, v->plate, 19);
            ts->lat         = g_facilities[FACILITY_GARAGE].lat;
            ts->lon         = g_facilities[FACILITY_GARAGE].lon;
            ts->prev_lat    = ts->lat;
            ts->prev_lon    = ts->lon;
            ts->step_index  = 0;
            ts->total_steps = rt->bin_count + 2; /* garage + bins + disposal */
        }

        log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
            "\n  [ROUTE %d] V%d %s | %d bins | %.1fkm | %.2fL fuel\n",
            rt->route_id, v->vehicle_id, v->plate,
            rt->bin_count, rt->total_distance, rt->fuel_required);

        /* Simulate each bin collection in order */
        for(int b=0;b<rt->bin_count;b++){
            int bi=bin_find(rt->bin_sequence[b]); if(bi<0) continue;
            Bin *bin=&g_bins[bi]; if(bin->collected_today) continue;

            float waste_kg=bin->fill_level*bin->capacity_kg/100.0f;

            /* Check if load exceeds capacity → mid-trip disposal */
            if(waste_loaded+waste_kg > v->capacity_kg && waste_loaded>0){
                int mid_dfi=route_nearest_disposal(bin->waste_type,bin->lat,bin->lon);
                if(mid_dfi<0) mid_dfi=1;
                float d2d=route_haversine(bin->lat,bin->lon,
                    g_facilities[mid_dfi].lat,g_facilities[mid_dfi].lon);
                float glat=g_facilities[FACILITY_GARAGE].lat;
                float glon=g_facilities[FACILITY_GARAGE].lon;
                float d2g=route_haversine(g_facilities[mid_dfi].lat,g_facilities[mid_dfi].lon,glat,glon);
                vehicle_deduct_fuel(v->vehicle_id, d2d+d2g);
                vehicle_unload(v->vehicle_id);
                waste_loaded=0; disposal_trips++;
                log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
                    "    [MID-DISPOSAL] V%d → %s → Garage (%.1fkm) → resume\n",
                    v->vehicle_id,g_facilities[mid_dfi].name,d2d+d2g);
            }

            /* Collect bin */
            waste_loaded += waste_kg;
            vehicle_add_load(v->vehicle_id, waste_kg);

            if(!strcmp(bin->waste_type,"recycling")) recycling+=waste_kg;
            else if(!strcmp(bin->waste_type,"hazardous")) hazardous+=waste_kg;
            else if(!strcmp(bin->waste_type,"organic"))   organic+=waste_kg;
            else if(!strcmp(bin->waste_type,"dry"))       dry+=waste_kg;
            else if(!strcmp(bin->waste_type,"wet"))       wet+=waste_kg;
            else if(!strcmp(bin->waste_type,"mixed"))     mixed+=waste_kg;
            else                                          general+=waste_kg;

            bin->collected_today=1; bin->fill_level=0; bin->priority=0;
            bin->total_waste_collected+=waste_kg;
            strftime(bin->last_collected,sizeof(bin->last_collected),
                "%Y-%m-%d %H:%M:%S",localtime(&now_t));
            collected++;

            /* Record animation step: truck at this bin */
            if (g_sim_step_count < MAX_TRUCK_STEPS) {
                TruckStep *ts = &g_sim_steps[g_sim_step_count++];
                memset(ts, 0, sizeof(*ts));
                ts->route_id      = rt->route_id;
                ts->vehicle_id    = v->vehicle_id;
                strncpy(ts->plate, v->plate, 19);
                ts->lat           = bin->lat;
                ts->lon           = bin->lon;
                ts->prev_lat      = (b > 0) ? g_bins[bin_find(rt->bin_sequence[b-1])].lat
                                             : g_facilities[FACILITY_GARAGE].lat;
                ts->prev_lon      = (b > 0) ? g_bins[bin_find(rt->bin_sequence[b-1])].lon
                                             : g_facilities[FACILITY_GARAGE].lon;
                ts->bin_collected = bin->bin_id;
                ts->step_index    = b + 1;
                ts->total_steps   = rt->bin_count + 2;
            }

            /* Hazardous waste requires special vehicle */
            if (!strcmp(bin->waste_type, "hazardous")) {
                if (!v->hazard_capable) {
                    log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
                        "    [HAZARD SKIP] Bin%d is hazardous but V%d not hazard-capable!\n",
                        bin->bin_id, v->vehicle_id);
                    continue;
                }
                rt->has_hazardous = 1;
                g_today.hazard_collections++;
                log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
                    "    [HAZARD] ☢ Bin%d hazardous waste — risk flag set, slow pickup (+0.05h)\n",
                    bin->bin_id);
                /* Extra time cost for hazard handling */
                if (di >= 0) driver_log_hours(g_drivers[di].driver_id, 0.05f);
            }

            log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
                "    Bin%d [%s] %.1fkg %s ✓\n",
                bin->bin_id,bin->location,waste_kg,bin->waste_type);
        }

        /* Record animation: truck at disposal */
        {
            int dfi = rt->disposal_facility_id;
            if (dfi < 0 || dfi >= NUM_FACILITIES) dfi = FACILITY_DISPOSAL;
            if (g_sim_step_count < MAX_TRUCK_STEPS) {
                TruckStep *ts = &g_sim_steps[g_sim_step_count++];
                memset(ts, 0, sizeof(*ts));
                ts->route_id    = rt->route_id;
                ts->vehicle_id  = v->vehicle_id;
                strncpy(ts->plate, v->plate, 19);
                ts->lat         = g_facilities[dfi].lat;
                ts->lon         = g_facilities[dfi].lon;
                ts->prev_lat    = ts->lat;
                ts->prev_lon    = ts->lon;
                ts->step_index  = rt->bin_count + 1;
                ts->total_steps = rt->bin_count + 2;
                ts->done        = 1;
            }
        }

        /* Final leg: bins → disposal → garage */
        vehicle_deduct_fuel(v->vehicle_id, rt->dist_to_disposal+rt->dist_disposal_to_garage);
        /* Check if vehicle broke down on final leg */
        if (g_vehicles[vi].needs_maintenance && g_vehicles[vi].total_distance > 0) {
            log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
                "    [BREAKDOWN] V%d broke down on return — marked for service\n",
                v->vehicle_id);
            sim_log("WARN","VEHICLE","V%d broke down during route %d",
                v->vehicle_id, rt->route_id);
            vehicle_set_status(v->vehicle_id,"breakdown");
        }
        vehicle_unload(v->vehicle_id);
        vehicle_set_status(v->vehicle_id,"idle");
        g_vehicles[vi].assigned_route=0;

        /* Driver hours */
        float hours = rt->total_distance/35.0f + rt->bin_count*0.1f;
        if(di>=0) driver_log_hours(g_drivers[di].driver_id, hours);

        /* Costs */
        float fuel_used=rt->fuel_required;
        float fuel_cost=fuel_used*fuel_price;
        float lab_cost=(di>=0)?hours*g_drivers[di].salary_per_hour:0;
        rt->fuel_cost=fuel_cost; rt->labor_cost=lab_cost; rt->total_cost=fuel_cost+lab_cost;
        rt->total_waste_kg=waste_loaded;
        rt->completed=1;
        strftime(rt->completed_at,sizeof(rt->completed_at),"%Y-%m-%d %H:%M:%S",localtime(&now_t));

        /* Update daily stats */
        g_today.bins_collected    += collected;
        g_today.routes_completed  ++;
        g_today.total_distance_km += rt->total_distance;
        g_today.total_fuel_used   += fuel_used;
        g_today.total_fuel_cost   += fuel_cost;
        g_today.total_labor_cost  += lab_cost;
        g_today.total_cost        += fuel_cost+lab_cost;
        g_today.recycling_kg      += recycling;
        g_today.hazardous_kg      += hazardous;
        g_today.general_kg        += general;
        g_today.organic_kg        += organic;
        g_today.dry_kg            += dry;
        g_today.wet_kg            += wet;
        g_today.mixed_kg          += mixed;
        /* recyclable = dry + recycling; non-recyclable = wet+mixed+general+organic */
        g_today.recyclable_kg     += dry + recycling;
        g_today.non_recyclable_kg += wet + mixed + general + organic;
        g_today.landfill_kg       += wet + mixed + general + organic;

        /* CO2 tracking */
        float co2_route = fuel_used * CO2_PER_LITRE;
        rt->co2_kg               = co2_route;
        g_vehicles[vi].co2_kg   += co2_route;
        g_today.co2_total_kg    += co2_route;

        sr.bins_collected += collected;
        sr.disposal_trips += disposal_trips;
        sr.fuel_used      += fuel_used;
        sr.fuel_cost      += fuel_cost;
        sr.labor_cost     += lab_cost;
        sr.total_cost     += fuel_cost+lab_cost;
        sr.total_distance += rt->total_distance;
        sr.routes_run++;

        log_p+=snprintf(simlog+log_p,sizeof(simlog)-log_p,
            "    → %s → Garage | Collected=%d Fuel=%.2fL Cost=₹%.2f\n",
            rt->disposal_name,collected,fuel_used,fuel_cost+lab_cost);
    }

    g_today.simulations_run++;
    g_sim_count++;
    bins_update_all_priorities();
    heatmap_update();  /* rebuild zone scores after sim */
    storage_save_all();
    sim_log("EVENT","SIM","Sim #%d done: %d routes, %d bins, ₹%.2f",
        g_sim_count,sr.routes_run,sr.bins_collected,sr.total_cost);

    /* Write to file log */
    FILE *lf=fopen("logs/simulation.log","a");
    if(lf){fprintf(lf,"%s\n",simlog);fclose(lf);}

    sr.success=1;
    snprintf(sr.message,sizeof(sr.message),
        "Sim#%d: %d routes, %d bins collected, %d disposal trips, "
        "%.1fkm, %.2fL, ₹%.2f",
        g_sim_count,sr.routes_run,sr.bins_collected,sr.disposal_trips,
        sr.total_distance,sr.fuel_used,sr.total_cost);
    return sr;
}

/* ── Emergency ─────────────────────────────────────────────── */
EmergencyResult sim_emergency(int bin_id, const char *reason) {
    g_emergency_alerts++;
    g_today.emergency_count++;

    EmergencyResult er = routes_emergency(bin_id, reason);
    if(!er.success){ g_emergency_alerts--; g_today.emergency_count--; return er; }

    /* Immediately execute emergency route */
    int ri=route_find(er.route_id); if(ri<0){er.success=0;return er;}
    Route *rt=&g_routes[ri];
    int vi=vehicle_find(rt->vehicle_id);
    if(vi>=0){
        int bi=bin_find(rt->bin_sequence[0]);
        if(bi>=0){g_bins[bi].collected_today=1;g_bins[bi].fill_level=0;g_bins[bi].priority=0;}
        vehicle_deduct_fuel(rt->vehicle_id, rt->total_distance);
        vehicle_unload(rt->vehicle_id);
        g_vehicles[vi].assigned_route=0;
        rt->completed=1;
        time_t now=time(NULL);
        strftime(rt->completed_at,sizeof(rt->completed_at),"%Y-%m-%d %H:%M:%S",localtime(&now));
    }

    char logbuf[256];
    snprintf(logbuf,sizeof(logbuf),"EMERGENCY Bin=%d Reason=%s V=%d",
        bin_id,reason,rt->vehicle_id);
    FILE *ef=fopen("logs/emergency.log","a");
    if(ef){time_t n=time(NULL);char ts[20];strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",localtime(&n));
        fprintf(ef,"[%s] %s\n",ts,logbuf);fclose(ef);}
    sim_log("EVENT","EMERG","%s",logbuf);

    storage_save_all();
    return er;
}

/* ── Dashboard JSON ────────────────────────────────────────── */
char *sim_dashboard_json(void) {
    int critical=0,overflow=0,collected=0,avail_v=0;
    for(int i=0;i<g_bin_count;i++){
        if(g_bins[i].priority>=1) critical++;
        if(g_bins[i].priority==3) overflow++;
        if(g_bins[i].collected_today) collected++;
    }
    for(int i=0;i<g_vehicle_count;i++)
        if(g_vehicles[i].available&&!g_vehicles[i].assigned_route&&!g_vehicles[i].needs_maintenance)
            avail_v++;

    char *buf=malloc(1200); if(!buf)return NULL;
    snprintf(buf,1200,
        "{\"total_bins\":%d,\"critical_bins\":%d,\"overflow_bins\":%d,"
        "\"bins_collected_today\":%d,\"available_vehicles\":%d,"
        "\"total_vehicles\":%d,\"total_drivers\":%d,"
        "\"emergency_alerts\":%d,\"simulations_run\":%d,"
        "\"routes_today\":%d,\"bins_collected\":%d,"
        "\"total_distance\":%.1f,\"fuel_used\":%.2f,"
        "\"fuel_cost\":%.2f,\"labor_cost\":%.2f,\"total_cost\":%.2f,"
        "\"recycling_kg\":%.1f,\"hazardous_kg\":%.1f,"
        "\"general_kg\":%.1f,\"organic_kg\":%.1f,"
        "\"dry_kg\":%.1f,\"wet_kg\":%.1f,\"mixed_kg\":%.1f,"
        "\"current_day\":\"%s\",\"maintenance_events\":%d,\"co2_total_kg\":%.2f}",
        g_bin_count,critical,overflow,collected,avail_v,
        g_vehicle_count,g_driver_count,
        g_emergency_alerts,g_sim_count,
        g_today.routes_completed,g_today.bins_collected,
        g_today.total_distance_km,g_today.total_fuel_used,
        g_today.total_fuel_cost,g_today.total_labor_cost,g_today.total_cost,
        g_today.recycling_kg,g_today.hazardous_kg,
        g_today.general_kg,g_today.organic_kg,
        g_today.dry_kg,g_today.wet_kg,g_today.mixed_kg,
        g_today.date,g_today.maintenance_events,g_today.co2_total_kg);
    return buf;
}

/* ── Reports ───────────────────────────────────────────────── */
char *sim_report_daily_json(void){
    char *b=malloc(512); if(!b)return NULL;
    snprintf(b,512,
        "{\"date\":\"%s\",\"bins_collected\":%d,\"routes_completed\":%d,"
        "\"distance_km\":%.1f,\"fuel_used\":%.2f,\"fuel_cost\":%.2f,"
        "\"labor_cost\":%.2f,\"total_cost\":%.2f,\"emergencies\":%d,"
        "\"maintenance\":%d,\"simulations\":%d}",
        g_today.date,g_today.bins_collected,g_today.routes_completed,
        g_today.total_distance_km,g_today.total_fuel_used,
        g_today.total_fuel_cost,g_today.total_labor_cost,g_today.total_cost,
        g_today.emergency_count,g_today.maintenance_events,g_today.simulations_run);
    return b;
}
char *sim_report_fuel_json(void){
    char *b=malloc(512); if(!b)return NULL;
    int n=g_vehicle_count;
    snprintf(b,512,
        "{\"total_fuel_used\":%.2f,\"total_fuel_cost\":%.2f,"
        "\"vehicles\":%d,\"avg_fuel_per_vehicle\":%.2f,"
        "\"total_distance\":%.1f,\"avg_km_per_litre\":%.2f}",
        g_today.total_fuel_used,g_today.total_fuel_cost,n,
        n>0?g_today.total_fuel_used/n:0,
        g_today.total_distance_km,
        g_today.total_fuel_used>0?g_today.total_distance_km/g_today.total_fuel_used:0);
    return b;
}
char *sim_report_cost_json(void){
    char *b=malloc(512); if(!b)return NULL;
    snprintf(b,512,
        "{\"total_cost\":%.2f,\"fuel_cost\":%.2f,\"labor_cost\":%.2f,"
        "\"cost_per_bin\":%.2f,\"cost_per_km\":%.2f}",
        g_today.total_cost,g_today.total_fuel_cost,g_today.total_labor_cost,
        g_today.bins_collected>0?g_today.total_cost/g_today.bins_collected:0,
        g_today.total_distance_km>0?g_today.total_cost/g_today.total_distance_km:0);
    return b;
}
char *sim_report_zone_json(void){
    char zones[32][16]; int zb[32]={0},zc[32]={0},zcol[32]={0};int nz=0;
    for(int i=0;i<g_bin_count;i++){
        const char *z=g_bins[i].zone; int f=-1;
        for(int j=0;j<nz;j++) if(!strcmp(zones[j],z)){f=j;break;}
        if(f<0&&nz<32){strncpy(zones[nz],z,15);f=nz++;}
        if(f>=0){zb[f]++;if(g_bins[i].priority>=1)zc[f]++;if(g_bins[i].collected_today)zcol[f]++;}
    }
    char *b=malloc(4096); if(!b)return NULL;
    int p=snprintf(b,4096,"[");
    for(int i=0;i<nz;i++)
        p+=snprintf(b+p,4096-p,
            "%s{\"zone\":\"%s\",\"total\":%d,\"critical\":%d,\"collected\":%d,\"pending\":%d}",
            i?",":"",zones[i],zb[i],zc[i],zcol[i],zb[i]-zcol[i]);
    snprintf(b+p,4096-p,"]"); return b;
}
char *sim_report_recycling_json(void){
    /* Compute recyclable vs non-recyclable vs landfill */
    float recyclable     = g_today.dry_kg + g_today.recycling_kg; /* dry = recyclable */
    float non_recyclable = g_today.wet_kg + g_today.general_kg + g_today.organic_kg;
    float landfill       = non_recyclable; /* non-recyclable → landfill */
    float total = g_today.dry_kg + g_today.wet_kg + g_today.mixed_kg
                + g_today.hazardous_kg + g_today.recycling_kg
                + g_today.general_kg + g_today.organic_kg;
    /* Update live trackers */
    g_today.recyclable_kg     = recyclable;
    g_today.non_recyclable_kg = non_recyclable;
    g_today.landfill_kg       = landfill;
    /* CO2 saved by recycling: 0.3kg CO2 saved per kg recycled */
    float co2_saved = recyclable * 0.3f;
    float seg_pct   = total > 0 ? (g_today.dry_kg+g_today.wet_kg)/(total)*100.0f : 0;
    float recycling_pct = total > 0 ? recyclable/total*100.0f : 0;
    char *b=malloc(1024); if(!b)return NULL;
    snprintf(b,1024,
        "{"
        "\"recycling_kg\":%.1f,"
        "\"hazardous_kg\":%.1f,"
        "\"general_kg\":%.1f,"
        "\"organic_kg\":%.1f,"
        "\"dry_kg\":%.1f,"
        "\"wet_kg\":%.1f,"
        "\"mixed_kg\":%.1f,"
        "\"total_kg\":%.1f,"
        "\"recyclable_kg\":%.1f,"
        "\"non_recyclable_kg\":%.1f,"
        "\"landfill_kg\":%.1f,"
        "\"recycling_pct\":%.1f,"
        "\"segregation_pct\":%.1f,"
        "\"co2_saved_kg\":%.1f,"
        "\"co2_emitted_kg\":%.1f"
        "}",
        g_today.recycling_kg, g_today.hazardous_kg,
        g_today.general_kg, g_today.organic_kg,
        g_today.dry_kg, g_today.wet_kg, g_today.mixed_kg,
        total, recyclable, non_recyclable, landfill,
        recycling_pct, seg_pct, co2_saved, g_today.co2_total_kg);
    return b;
}
char *sim_report_maintenance_json(void){
    char *b=malloc(4096); if(!b)return NULL;
    int p=snprintf(b,4096,"[");
    for(int i=0;i<g_vehicle_count;i++){
        Vehicle *v=&g_vehicles[i];
        p+=snprintf(b+p,4096-p,
            "%s{\"vehicle_id\":%d,\"plate\":\"%s\","
            "\"lifetime_km\":%.0f,\"daily_km\":%.1f,"
            "\"maintenance_due_km\":%d,\"needs_maintenance\":%s,"
            "\"fuel_pct\":%.0f}",
            i?",":"",v->vehicle_id,v->plate,
            v->lifetime_km,v->total_distance,
            v->maintenance_due_km,
            v->needs_maintenance?"true":"false",
            v->fuel_capacity>0?v->fuel_level/v->fuel_capacity*100:0);
        if(p>3800)break;
    }
    snprintf(b+p,4096-p,"]"); return b;
}

char *sim_log_json(int last_n){
    int start=0;
    if(last_n>0 && g_log_count>last_n) start=g_log_count-last_n;
    int cnt=g_log_count-start;
    int sz=128+cnt*350; char *b=malloc(sz); if(!b)return NULL;
    int p=snprintf(b,sz,"[");
    for(int i=start;i<g_log_count;i++){
        LogEntry *e=&g_log[i];
        p+=snprintf(b+p,sz-p,
            "%s{\"ts\":\"%s\",\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\"}",
            i>start?",":"",e->timestamp,e->level,e->module,e->message);
        if(p>sz-300)break;
    }
    snprintf(b+p,sz-p,"]"); return b;
}
