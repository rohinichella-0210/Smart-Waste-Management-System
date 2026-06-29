#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "types.h"
#include "storage.h"
#include "bins.h"
#include "vehicles.h"
#include "drivers.h"
#include "routes.h"
#include "simulation.h"

static char g_data[256]="data";
static char g_logs[256]="logs";

void storage_init(const char *d, const char *l){
    if(d) strncpy(g_data,d,255);
    if(l) strncpy(g_logs,l,255);
    char cmd[512];
    #ifdef _WIN32
    snprintf(cmd,sizeof(cmd),"mkdir %s 2>nul & mkdir %s 2>nul",g_data,g_logs);
#else
    snprintf(cmd,sizeof(cmd),"mkdir -p %s 2>/dev/null && mkdir -p %s 2>/dev/null",g_data,g_logs);
#endif
    if(system(cmd)){}
}

void storage_save_bins(void){
    char p[512]; snprintf(p,sizeof(p),"%s/bins.csv",g_data);
    FILE *f=fopen(p,"w"); if(!f)return;
    fprintf(f,"bin_id,location,fill_level,capacity_kg,waste_type,threshold,zone,lat,lon,fill_rate,collected_today,last_collected,total_waste_collected\n");
    for(int i=0;i<g_bin_count;i++){
        Bin *b=&g_bins[i];
        fprintf(f,"%d,%s,%.1f,%.0f,%s,%d,%s,%.6f,%.6f,%.2f,%d,%s,%.1f\n",
            b->bin_id,b->location,b->fill_level,b->capacity_kg,
            b->waste_type,b->threshold,b->zone,b->lat,b->lon,
            b->fill_rate,b->collected_today,b->last_collected,b->total_waste_collected);
    }
    fclose(f);
}

void storage_save_vehicles(void){
    char p[512]; snprintf(p,sizeof(p),"%s/vehicles.csv",g_data);
    FILE *f=fopen(p,"w"); if(!f)return;
    fprintf(f,"vehicle_id,plate,vehicle_class,capacity_kg,current_load,fuel_level,fuel_capacity,fuel_rate,zone,available,assigned_route,driver_id,total_distance,lifetime_km,needs_maintenance,maintenance_due_km,status\n");
    for(int i=0;i<g_vehicle_count;i++){
        Vehicle *v=&g_vehicles[i];
        fprintf(f,"%d,%s,%s,%.0f,%.1f,%.1f,%.0f,%.3f,%s,%d,%d,%d,%.1f,%.0f,%d,%d,%s\n",
            v->vehicle_id,v->plate,v->vehicle_class,
            v->capacity_kg,v->current_load,v->fuel_level,v->fuel_capacity,
            v->fuel_rate,v->zone,v->available,v->assigned_route,v->driver_id,
            v->total_distance,v->lifetime_km,v->needs_maintenance,
            v->maintenance_due_km,v->status);
    }
    fclose(f);
}

void storage_save_drivers(void){
    char p[512]; snprintf(p,sizeof(p),"%s/drivers.csv",g_data);
    FILE *f=fopen(p,"w"); if(!f)return;
    fprintf(f,"driver_id,name,license,shift,is_substitute,vehicle_id,hours_worked,max_hours,overtime_hours,salary_per_hour,available,on_leave,leave_reason,absent_days\n");
    for(int i=0;i<g_driver_count;i++){
        Driver *d=&g_drivers[i];
        fprintf(f,"%d,%s,%s,%s,%d,%d,%.1f,%.0f,%.1f,%.2f,%d,%d,%s,%d\n",
            d->driver_id,d->name,d->license,d->shift,d->is_substitute,
            d->vehicle_id,d->hours_worked,d->max_hours,d->overtime_hours,
            d->salary_per_hour,d->available,d->on_leave,
            d->leave_reason[0]?d->leave_reason:"none",d->absent_days);
    }
    fclose(f);
}

void storage_save_routes(void){
    char p[512]; snprintf(p,sizeof(p),"%s/routes.csv",g_data);
    FILE *f=fopen(p,"w"); if(!f)return;
    fprintf(f,"route_id,vehicle_id,zone,bin_count,bins,total_distance,fuel_required,total_waste_kg,fuel_cost,labor_cost,total_cost,disposal_site,completed,emergency,mid_disposal,route_string,created_at,completed_at\n");
    for(int i=0;i<g_route_count;i++){
        Route *r=&g_routes[i];
        char seq[256]=""; int sp=0;
        for(int j=0;j<r->bin_count;j++) sp+=snprintf(seq+sp,sizeof(seq)-sp,"%s%d",j?";":"",r->bin_sequence[j]);
        fprintf(f,"%d,%d,%s,%d,%s,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f,%s,%d,%d,%d,\"%s\",%s,%s\n",
            r->route_id,r->vehicle_id,r->zone,r->bin_count,seq,
            r->total_distance,r->fuel_required,r->total_waste_kg,
            r->fuel_cost,r->labor_cost,r->total_cost,
            r->disposal_name,r->completed,r->is_emergency,r->mid_trip_disposal,
            r->route_string,r->created_at,r->completed_at);
    }
    fclose(f);
}

void storage_save_stats(void){
    char p[512]; snprintf(p,sizeof(p),"%s/stats.csv",g_data);
    FILE *f=fopen(p,"w"); if(!f)return;
    fprintf(f,"date,simulations,bins_collected,routes,distance,fuel_used,fuel_cost,labor_cost,total_cost,emergencies,maintenance,recycling_kg,hazardous_kg,general_kg,organic_kg\n");
    /* Save today */
    DailyStats *s=&g_today;
    fprintf(f,"%s,%d,%d,%d,%.1f,%.2f,%.2f,%.2f,%.2f,%d,%d,%.1f,%.1f,%.1f,%.1f\n",
        s->date,s->simulations_run,s->bins_collected,s->routes_completed,
        s->total_distance_km,s->total_fuel_used,s->total_fuel_cost,
        s->total_labor_cost,s->total_cost,s->emergency_count,s->maintenance_events,
        s->recycling_kg,s->hazardous_kg,s->general_kg,s->organic_kg);
    fclose(f);
}

void storage_save_log(void){
    char p[512]; snprintf(p,sizeof(p),"%s/app.log",g_logs);
    FILE *f=fopen(p,"a"); if(!f)return;
    /* Append only new entries — write last 50 entries */
    int start=g_log_count>50?g_log_count-50:0;
    for(int i=start;i<g_log_count;i++){
        LogEntry *e=&g_log[i];
        fprintf(f,"[%s][%s][%s] %s\n",e->timestamp,e->level,e->module,e->message);
    }
    fclose(f);
}

void storage_save_all(void){
    storage_save_bins(); storage_save_vehicles(); storage_save_drivers();
    storage_save_routes(); storage_save_stats(); storage_save_log();
}

/* ── Loaders ─────────────────────────────────────────────── */
static void load_bins(void){
    char p[512]; snprintf(p,sizeof(p),"%s/bins.csv",g_data);
    FILE *f=fopen(p,"r"); if(!f)return;
    char line[512]; if(!fgets(line,sizeof(line),f)){fclose(f);return;}
    while(fgets(line,sizeof(line),f)&&g_bin_count<MAX_BINS){
        Bin b={0};
        if(sscanf(line,"%d,%63[^,],%f,%f,%15[^,],%d,%15[^,],%f,%f,%f,%d",
            &b.bin_id,b.location,&b.fill_level,&b.capacity_kg,
            b.waste_type,&b.threshold,b.zone,&b.lat,&b.lon,&b.fill_rate,
            &b.collected_today)<7) continue;
        if(b.capacity_kg<=0)b.capacity_kg=500;
        if(b.fill_rate<=0)b.fill_rate=8;
        g_bins[g_bin_count]=b;
        bin_update_priority(g_bin_count);
        g_bin_count++;
    }
    fclose(f);
}

static void load_vehicles(void){
    char p[512]; snprintf(p,sizeof(p),"%s/vehicles.csv",g_data);
    FILE *f=fopen(p,"r"); if(!f)return;
    char line[512]; if(!fgets(line,sizeof(line),f)){fclose(f);return;}
    while(fgets(line,sizeof(line),f)&&g_vehicle_count<MAX_VEHICLES){
        Vehicle v={0};
        if(sscanf(line,"%d,%19[^,],%15[^,],%f,%f,%f,%f,%f,%15[^,],%d,%d,%d,%f,%f,%d,%d,%31[^\n]",
            &v.vehicle_id,v.plate,v.vehicle_class,
            &v.capacity_kg,&v.current_load,&v.fuel_level,&v.fuel_capacity,
            &v.fuel_rate,v.zone,&v.available,&v.assigned_route,&v.driver_id,
            &v.total_distance,&v.lifetime_km,&v.needs_maintenance,
            &v.maintenance_due_km,v.status)<9) continue;
        if(v.fuel_rate<=0)v.fuel_rate=0.12f;
        if(v.maintenance_due_km<=0)v.maintenance_due_km=500;
        if(!v.vehicle_class[0])strncpy(v.vehicle_class,"normal",15);
        v.current_lat=g_facilities[FACILITY_GARAGE].lat;
        v.current_lon=g_facilities[FACILITY_GARAGE].lon;
        g_vehicles[g_vehicle_count++]=v;
    }
    fclose(f);
}

static void load_drivers(void){
    char p[512]; snprintf(p,sizeof(p),"%s/drivers.csv",g_data);
    FILE *f=fopen(p,"r"); if(!f)return;
    char line[512]; if(!fgets(line,sizeof(line),f)){fclose(f);return;}
    while(fgets(line,sizeof(line),f)&&g_driver_count<MAX_DRIVERS){
        Driver d={0};
        int matched = sscanf(line,
            "%d,%63[^,],%31[^,],%15[^,],%d,%d,%f,%f,%f,%f,%d,%d,%47[^,],%d",
            &d.driver_id,d.name,d.license,d.shift,&d.is_substitute,
            &d.vehicle_id,&d.hours_worked,&d.max_hours,&d.overtime_hours,
            &d.salary_per_hour,&d.available,&d.on_leave,d.leave_reason,&d.absent_days);
        if(matched < 4) continue;
        if(d.salary_per_hour<=0)d.salary_per_hour=15;
        if(d.max_hours<=0)d.max_hours=d.is_substitute?4:8;
        g_drivers[g_driver_count++]=d;
    }
    fclose(f);
}

static void load_routes(void){
    char p[512]; snprintf(p,sizeof(p),"%s/routes.csv",g_data);
    FILE *f=fopen(p,"r"); if(!f)return;
    char line[1200]; if(!fgets(line,sizeof(line),f)){fclose(f);return;}
    while(fgets(line,sizeof(line),f)&&g_route_count<MAX_ROUTES){
        Route r={0}; char seq[256]="";
        if(sscanf(line,"%d,%d,%15[^,],%d,%255[^,],%f,%f,%f,%f,%f,%f",
            &r.route_id,&r.vehicle_id,r.zone,&r.bin_count,seq,
            &r.total_distance,&r.fuel_required,&r.total_waste_kg,
            &r.fuel_cost,&r.labor_cost,&r.total_cost)<6) continue;
        /* Parse bin sequence */
        char *tok=strtok(seq,";"); int j=0;
        while(tok&&j<MAX_BINS_PER_RT){r.bin_sequence[j++]=atoi(tok);tok=strtok(NULL,";");}
        /* completed flag */
        char *cp=line; for(int k=0;k<12;k++){cp=strchr(cp,',');if(!cp)break;cp++;}
        if(cp) sscanf(cp,"%d,%d,%d",&r.completed,&r.is_emergency,&r.mid_trip_disposal);
        g_routes[g_route_count++]=r;
    }
    fclose(f);
}

static void load_stats(void){
    char p[512]; snprintf(p,sizeof(p),"%s/stats.csv",g_data);
    FILE *f=fopen(p,"r"); if(!f)return;
    char line[512]; if(!fgets(line,sizeof(line),f)){fclose(f);return;}
    if(fgets(line,sizeof(line),f)){
        DailyStats s={0};
        sscanf(line,"%19[^,],%d,%d,%d,%f,%f,%f,%f,%f,%d,%d,%f,%f,%f,%f",
            s.date,&s.simulations_run,&s.bins_collected,&s.routes_completed,
            &s.total_distance_km,&s.total_fuel_used,&s.total_fuel_cost,
            &s.total_labor_cost,&s.total_cost,&s.emergency_count,&s.maintenance_events,
            &s.recycling_kg,&s.hazardous_kg,&s.general_kg,&s.organic_kg);
        g_today=s;
    }
    fclose(f);
}

void storage_load_all(void){
    load_bins(); load_vehicles(); load_drivers(); load_routes(); load_stats();
    bins_update_all_priorities();
    printf("[Storage] Bins:%d Vehicles:%d Drivers:%d Routes:%d\n",
        g_bin_count,g_vehicle_count,g_driver_count,g_route_count);
}
