#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib,"ws2_32.lib")
  #define CLOSESOCK closesocket
  typedef SOCKET SOCK_T;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #define CLOSESOCK close
  typedef int SOCK_T;
  #define INVALID_SOCKET -1
#endif

#include "types.h"
#include "bins.h"
#include "vehicles.h"
#include "drivers.h"
#include "routes.h"
#include "simulation.h"
#include "storage.h"
#include "validation.h"
#include "auth.h"
#include "heatmap.h"
#include "citizen.h"

#define PORT 8080
#define RBUF 16384

/* ── HTTP helpers ──────────────────────────────────────────── */
static void hdr(SOCK_T s, int code, const char *ct, int len){
    char b[600];
    const char *st="OK";
    if(code==400)st="Bad Request";
    else if(code==404)st="Not Found";
    else if(code==405)st="Method Not Allowed";
    snprintf(b,sizeof(b),
        "HTTP/1.1 %d %s\r\nContent-Type: %s; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Cache-Control: no-store\r\n\r\n",
        code,st,ct,len);
    send(s,b,(int)strlen(b),0);
}
static void resp(SOCK_T s,int code,const char *body){
    hdr(s,code,"application/json",(int)strlen(body));
    send(s,body,(int)strlen(body),0);
}
static void resp_free(SOCK_T s,int code,char *body){
    if(!body){resp(s,500,"{\"error\":\"oom\"}");return;}
    resp(s,code,body); free(body);
}

typedef struct { char method[8];char path[256];char body[RBUF];int blen; } Req;

static int parse(const char *raw,int n,Req *r){
    memset(r,0,sizeof(*r));
    int i=0;
    while(raw[i]&&raw[i]!=' '&&i<7){r->method[i]=raw[i];i++;}
    r->method[i]='\0'; if(raw[i]!=' ')return -1; i++;
    int j=0; while(raw[i]&&raw[i]!=' '&&j<255){r->path[j++]=raw[i++];}
    r->path[j]='\0';
    /* Strip query string */
    char *qs=strchr(r->path,'?'); if(qs)*qs='\0';
    const char *sep=strstr(raw,"\r\n\r\n");
    if(sep){sep+=4;r->blen=(int)(raw+n-sep);
        if(r->blen>RBUF-1)r->blen=RBUF-1;
        memcpy(r->body,sep,r->blen);r->body[r->blen]='\0';}
    return 0;
}

/* ── Handlers ─────────────────────────────────────────────── */
static void h_bins(SOCK_T s,Req *r){
    if(!strcmp(r->method,"GET")){ resp_free(s,200,bins_to_json()); return; }
    if(!strcmp(r->method,"POST")){
        Bin b={0};
        jint(r->body,"bin_id",&b.bin_id);
        jstr(r->body,"location",b.location,64);
        jstr(r->body,"zone",b.zone,16);
        jstr(r->body,"waste_type",b.waste_type,16);
        jflt(r->body,"capacity_kg",&b.capacity_kg);
        jflt(r->body,"fill_level",&b.fill_level);
        jint(r->body,"threshold",&b.threshold);
        jflt(r->body,"lat",&b.lat); jflt(r->body,"lon",&b.lon);
        jflt(r->body,"fill_rate",&b.fill_rate);
        /* ── Input Validation ── */
        if(b.bin_id<=0)
            {resp(s,400,"{\"error\":\"bin_id must be a positive integer\"}");return;}
        if(!b.location[0])
            {resp(s,400,"{\"error\":\"location (bin ID) cannot be empty\"}");return;}
        if(b.fill_level<0||b.fill_level>100)
            {resp(s,400,"{\"error\":\"fill_level must be between 0 and 100\"}");return;}
        if(b.capacity_kg<0)
            {resp(s,400,"{\"error\":\"capacity_kg cannot be negative\"}");return;}
        if(b.waste_type[0] && !waste_type_valid(b.waste_type)){
            resp(s,400,"{\"error\":\"waste_type must be one of: dry, wet, hazardous, mixed\"}");return;
        }
        b.fill_level=fclamp(b.fill_level,0,100);
        int rc=bin_add(&b);
        if(rc==-1){resp(s,400,"{\"error\":\"Duplicate bin_id\"}");return;}
        if(rc==-4){resp(s,400,"{\"error\":\"bin_id must be a positive integer\"}");return;}
        if(rc==-5){resp(s,400,"{\"error\":\"location cannot be empty\"}");return;}
        if(rc==-6){resp(s,400,"{\"error\":\"capacity_kg cannot be negative\"}");return;}
        resp(s,200,"{\"success\":true,\"message\":\"Bin added\"}"); return;
    }
    if(!strcmp(r->method,"PUT")){
        /* Update fill level (set absolute %) */
        int bid=0; float fill=0; int col=0;
        jint(r->body,"bin_id",&bid);
        jflt(r->body,"fill_level",&fill);
        jint(r->body,"collected_today",&col);
        if(bin_set_fill(bid,fill)<0){resp(s,404,"{\"error\":\"Bin not found\"}");return;}
        resp(s,200,"{\"success\":true}"); return;
    }
    if(!strcmp(r->method,"DELETE")){
        int bid=0; jint(r->body,"bin_id",&bid);
        if(bin_delete(bid)<0){resp(s,404,"{\"error\":\"Bin not found\"}");return;}
        resp(s,200,"{\"success\":true}"); return;
    }
    resp(s,405,"{\"error\":\"method not allowed\"}");
}

static void h_add_waste(SOCK_T s,Req *r){
    /* POST /api/bins/addwaste — add kg of waste to existing bin in real-time */
    if(strcmp(r->method,"POST")){resp(s,405,"{\"error\":\"POST required\"}");return;}
    int bid=0; float kg=0;
    jint(r->body,"bin_id",&bid);
    jflt(r->body,"kg",&kg);
    if(bid<=0||kg<=0){resp(s,400,"{\"error\":\"bin_id and kg required\"}");return;}
    int rc=sim_add_waste(bid,kg);
    if(rc<0){resp(s,404,"{\"error\":\"Bin not found\"}");return;}
    /* Return updated bin */
    int idx=bin_find(bid);
    char buf[256];
    snprintf(buf,sizeof(buf),
        "{\"success\":true,\"bin_id\":%d,\"fill_level\":%.1f,\"priority\":%d}",
        bid, idx>=0?g_bins[idx].fill_level:0, idx>=0?g_bins[idx].priority:0);
    resp(s,200,buf);
}

static void h_vehicles(SOCK_T s,Req *r){
    if(!strcmp(r->method,"GET")){ resp_free(s,200,vehicles_to_json()); return; }
    if(!strcmp(r->method,"POST")){
        Vehicle v={0};
        jint(r->body,"vehicle_id",&v.vehicle_id);
        jstr(r->body,"plate",v.plate,20);
        jstr(r->body,"vehicle_class",v.vehicle_class,16);
        jstr(r->body,"zone",v.zone,16);
        jflt(r->body,"capacity_kg",&v.capacity_kg);
        jflt(r->body,"fuel_level",&v.fuel_level);
        jflt(r->body,"fuel_capacity",&v.fuel_capacity);
        jflt(r->body,"fuel_rate",&v.fuel_rate);
        jint(r->body,"driver_id",&v.driver_id);
        jint(r->body,"maintenance_due_km",&v.maintenance_due_km);
        v.available=1;
        if(v.vehicle_id<=0){resp(s,400,"{\"error\":\"vehicle_id required\"}");return;}
        if(!v.vehicle_class[0]) strncpy(v.vehicle_class,"normal",15);
        int rc=vehicle_add(&v);
        if(rc==-1){resp(s,400,"{\"error\":\"Duplicate vehicle_id\"}");return;}
        resp(s,200,"{\"success\":true,\"message\":\"Vehicle added\"}"); return;
    }
    if(!strcmp(r->method,"PUT")){
        /* Refuel */
        int vid=0; float litres=0;
        jint(r->body,"vehicle_id",&vid); jflt(r->body,"litres",&litres);
        /* Service/maintenance */
        int service=0; jint(r->body,"service",&service);
        if(service) vehicle_service(vid);
        if(litres>0 && vehicle_refuel(vid,litres)<0){resp(s,404,"{\"error\":\"Vehicle not found\"}");return;}
        resp(s,200,"{\"success\":true,\"message\":\"Updated\"}"); return;
    }
    if(!strcmp(r->method,"DELETE")){
        int vid=0; jint(r->body,"vehicle_id",&vid);
        if(vehicle_delete(vid)<0){resp(s,404,"{\"error\":\"Vehicle not found\"}");return;}
        resp(s,200,"{\"success\":true}"); return;
    }
    resp(s,405,"{\"error\":\"method not allowed\"}");
}

static void h_drivers(SOCK_T s,Req *r){
    if(!strcmp(r->method,"GET")){ resp_free(s,200,drivers_to_json()); return; }
    if(!strcmp(r->method,"POST")){
        Driver d={0};
        jint(r->body,"driver_id",&d.driver_id);
        jstr(r->body,"name",d.name,64);
        jstr(r->body,"license",d.license,32);
        jstr(r->body,"shift",d.shift,16);
        jint(r->body,"is_substitute",&d.is_substitute);
        jint(r->body,"vehicle_id",&d.vehicle_id);
        jflt(r->body,"salary_per_hour",&d.salary_per_hour);
        jflt(r->body,"max_hours",&d.max_hours);
        /* ── Input Validation ── */
        if(d.driver_id<=0){resp(s,400,"{\"error\":\"driver_id must be a positive integer\"}");return;}
        if(!d.name[0]){resp(s,400,"{\"error\":\"driver name cannot be empty\"}");return;}
        /* Validate shift — only morning/evening allowed */
        if(d.shift[0] && strcmp(d.shift,"morning")!=0 && strcmp(d.shift,"evening")!=0){
            resp(s,400,"{\"error\":\"shift must be 'morning' or 'evening'\"}");return;
        }
        /* Prevent assigning driver to a vehicle already assigned to another driver */
        if(d.vehicle_id > 0){
            for(int i=0;i<g_driver_count;i++){
                if(g_drivers[i].vehicle_id==d.vehicle_id && g_drivers[i].available && !g_drivers[i].on_leave){
                    char eb[128];
                    snprintf(eb,128,"{\"error\":\"Vehicle %d is already assigned to Driver %d (%s)\"}",
                        d.vehicle_id, g_drivers[i].driver_id, g_drivers[i].name);
                    resp(s,400,eb); return;
                }
            }
        }
        int rc=driver_add(&d);
        if(rc==-1){resp(s,400,"{\"error\":\"Duplicate driver_id\"}");return;}
        /* Redistribute shifts if no shift specified */
        if(!d.shift[0]) drivers_assign_shifts();
        resp(s,200,"{\"success\":true,\"message\":\"Driver added\"}"); return;
    }
    if(!strcmp(r->method,"PUT")){
        int did=0,vid=0;
        jint(r->body,"driver_id",&did); jint(r->body,"vehicle_id",&vid);
        driver_assign_vehicle(did,vid);
        resp(s,200,"{\"success\":true}"); return;
    }
    if(!strcmp(r->method,"DELETE")){
        int did=0; jint(r->body,"driver_id",&did);
        if(driver_delete(did)<0){resp(s,404,"{\"error\":\"Driver not found\"}");return;}
        resp(s,200,"{\"success\":true}"); return;
    }
    resp(s,405,"{\"error\":\"method not allowed\"}");
}

static void h_simulate(SOCK_T s,Req *r){
    if(strcmp(r->method,"POST")){resp(s,405,"{\"error\":\"POST required\"}");return;}
    float fp=1.20f; jflt(r->body,"fuel_price",&fp);
    fp=fclamp(fp,0.1f,50.0f);

    SimResult sr=sim_run(fp);

    char *out=malloc(1024); if(!out){resp(s,500,"{\"error\":\"oom\"}");return;}
    snprintf(out,1024,
        "{\"success\":%s,\"routes_run\":%d,\"bins_collected\":%d,"
        "\"disposal_trips\":%d,\"total_distance\":%.1f,"
        "\"fuel_used\":%.2f,\"fuel_cost\":%.2f,"
        "\"labor_cost\":%.2f,\"total_cost\":%.2f,"
        "\"message\":\"%s\"}",
        sr.success?"true":"false",
        sr.routes_run,sr.bins_collected,sr.disposal_trips,
        sr.total_distance,sr.fuel_used,sr.fuel_cost,
        sr.labor_cost,sr.total_cost,sr.message);
    resp_free(s,200,out);
}

static void h_newday(SOCK_T s,Req *r){
    (void)r;
    sim_new_day();
    resp(s,200,"{\"success\":true,\"message\":\"New day reset: bins refilled, vehicles refuelled, drivers rested, shifts redistributed\"}");
}

static void h_emergency(SOCK_T s,Req *r){
    if(strcmp(r->method,"POST")){resp(s,405,"{\"error\":\"POST required\"}");return;}
    int bid=0; char reason[128]="overflow";
    jint(r->body,"bin_id",&bid);
    jstr(r->body,"reason",reason,128);
    if(bid<=0){resp(s,400,"{\"error\":\"bin_id required\"}");return;}
    EmergencyResult er=sim_emergency(bid,reason);
    char buf[512];
    snprintf(buf,sizeof(buf),
        "{\"success\":%s,\"vehicle_id\":%d,\"route_id\":%d,"
        "\"distance\":%.2f,\"message\":\"%s\"}",
        er.success?"true":"false",er.vehicle_id,er.route_id,
        er.distance,er.message);
    resp(s,200,buf);
}

/* GET /api/constraints — live constraint violation checker */
static void h_constraints(SOCK_T s){
    char *buf = malloc(8192); if(!buf){resp(s,500,"{\"error\":\"oom\"}");return;}
    int p = snprintf(buf,8192,"{\"violations\":[");
    int n = 0;

    /* 1. Same vehicle on multiple active routes */
    for(int i=0;i<g_route_count;i++){
        if(g_routes[i].completed) continue;
        for(int j=i+1;j<g_route_count;j++){
            if(g_routes[j].completed) continue;
            if(g_routes[i].vehicle_id==g_routes[j].vehicle_id){
                p+=snprintf(buf+p,8192-p,"%s{\"type\":\"DOUBLE_VEHICLE\","
                    "\"severity\":\"ERROR\","
                    "\"message\":\"Vehicle V%d assigned to routes #%d and #%d simultaneously\"}",
                    n?",":"",g_routes[i].vehicle_id,g_routes[i].route_id,g_routes[j].route_id);
                n++;
            }
        }
    }
    /* 2. Driver assigned to multiple vehicles */
    for(int i=0;i<g_driver_count;i++){
        if(g_drivers[i].vehicle_id<=0||!g_drivers[i].available) continue;
        for(int j=i+1;j<g_driver_count;j++){
            if(g_drivers[j].vehicle_id==g_drivers[i].vehicle_id && g_drivers[j].available){
                p+=snprintf(buf+p,8192-p,"%s{\"type\":\"DOUBLE_DRIVER\","
                    "\"severity\":\"ERROR\","
                    "\"message\":\"Drivers D%d and D%d both assigned to Vehicle V%d\"}",
                    n?",":"",g_drivers[i].driver_id,g_drivers[j].driver_id,g_drivers[i].vehicle_id);
                n++;
            }
        }
    }
    /* 3. Vehicle needs maintenance but is on active route */
    for(int i=0;i<g_vehicle_count;i++){
        if(g_vehicles[i].needs_maintenance && g_vehicles[i].assigned_route){
            p+=snprintf(buf+p,8192-p,"%s{\"type\":\"MAINTENANCE_BLOCKED\","
                "\"severity\":\"WARN\","
                "\"message\":\"V%d (%s) needs maintenance but is on an active route\"}",
                n?",":"",g_vehicles[i].vehicle_id,g_vehicles[i].plate);
            n++;
        }
    }
    /* 4. Driver overtime exceeded */
    for(int i=0;i<g_driver_count;i++){
        Driver *d=&g_drivers[i];
        if(d->hours_worked>=d->max_hours && d->available){
            p+=snprintf(buf+p,8192-p,"%s{\"type\":\"DRIVER_OVERTIME\","
                "\"severity\":\"WARN\","
                "\"message\":\"Driver D%d %s exceeded max hours (%.1f/%.0fh)\"}",
                n?",":"",d->driver_id,d->name,d->hours_worked,d->max_hours);
            n++;
        }
    }
    /* 5. Vehicle fuel critically low */
    for(int i=0;i<g_vehicle_count;i++){
        Vehicle *v=&g_vehicles[i];
        if(v->available && !v->needs_maintenance && v->fuel_capacity>0 && v->fuel_level < v->fuel_capacity*0.1f){
            p+=snprintf(buf+p,8192-p,"%s{\"type\":\"FUEL_INSUFFICIENT\","
                "\"severity\":\"WARN\","
                "\"message\":\"V%d (%s) fuel critically low: %.1fL / %.0fL\"}",
                n?",":"",v->vehicle_id,v->plate,v->fuel_level,v->fuel_capacity);
            n++;
        }
    }
    /* 6. Vehicle overloaded */
    for(int i=0;i<g_vehicle_count;i++){
        Vehicle *v=&g_vehicles[i];
        if(v->current_load > v->capacity_kg){
            p+=snprintf(buf+p,8192-p,"%s{\"type\":\"VEHICLE_OVERLOAD\","
                "\"severity\":\"ERROR\","
                "\"message\":\"V%d (%s) overloaded: %.0fkg / %.0fkg capacity\"}",
                n?",":"",v->vehicle_id,v->plate,v->current_load,v->capacity_kg);
            n++;
        }
    }
    /* 7. Overflow bins unserviced */
    for(int i=0;i<g_bin_count;i++){
        Bin *b=&g_bins[i];
        if(b->priority>=3 && !b->collected_today){
            p+=snprintf(buf+p,8192-p,"%s{\"type\":\"OVERFLOW_UNSERVICED\","
                "\"severity\":\"ERROR\","
                "\"message\":\"Bin %d [%s] is OVERFLOW and not yet collected\"}",
                n?",":"",b->bin_id,b->location);
            n++;
        }
    }
    /* 8. Hazardous bins pending without a capable vehicle */
    int haz_v=0;
    for(int i=0;i<g_vehicle_count;i++)
        if((g_vehicles[i].hazard_capable||!strcmp(g_vehicles[i].vehicle_class,"hazardous"))
           && g_vehicles[i].available && !g_vehicles[i].needs_maintenance) haz_v++;
    int haz_b=0;
    for(int i=0;i<g_bin_count;i++)
        if(!strcmp(g_bins[i].waste_type,"hazardous") && !g_bins[i].collected_today && g_bins[i].priority>=1) haz_b++;
    if(haz_b>0 && haz_v==0){
        p+=snprintf(buf+p,8192-p,"%s{\"type\":\"NO_HAZARD_VEHICLE\","
            "\"severity\":\"CRITICAL\","
            "\"message\":\"%d hazardous bin(s) need collection but no hazardous vehicle is available\"}",
            n?",":"",haz_b);
        n++;
    }

    snprintf(buf+p,8192-p,"],\"count\":%d,\"ok\":%s}",n,n==0?"true":"false");
    resp_free(s,200,buf);
}

static void h_routes(SOCK_T s,Req *r){
    if(!strcmp(r->method,"GET")){ resp_free(s,200,routes_to_json()); return; }
    if(!strcmp(r->method,"DELETE")){ routes_clear_completed(); resp(s,200,"{\"success\":true}"); return; }
    resp(s,405,"{\"error\":\"method not allowed\"}");
}

static void h_dashboard(SOCK_T s){ resp_free(s,200,sim_dashboard_json()); }

static void h_log(SOCK_T s,Req *r){
    int n=100; jint(r->body,"n",&n);
    resp_free(s,200,sim_log_json(n));
}

static void h_facilities(SOCK_T s){
    char buf[1024]; int p=snprintf(buf,sizeof(buf),"[");
    const char *ftypes[]={"garage","disposal","recycling","hazardous"};
    for(int i=0;i<NUM_FACILITIES;i++){
        Facility *f=&g_facilities[i];
        p+=snprintf(buf+p,sizeof(buf)-p,
            "%s{\"id\":%d,\"name\":\"%s\",\"type\":\"%s\","
            "\"lat\":%.6f,\"lon\":%.6f}",
            i?",":"",f->id,f->name,ftypes[f->type<4?f->type:0],f->lat,f->lon);
    }
    snprintf(buf+p,sizeof(buf)-p,"]"); resp(s,200,buf);
}

static void serve_file(SOCK_T s,const char *path){
    char fp[512]; snprintf(fp,sizeof(fp),"frontend%s",path);
    FILE *f=fopen(fp,"rb"); if(!f){resp(s,404,"{\"error\":\"not found\"}");return;}
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf=malloc(sz+1); if(!buf){fclose(f);resp(s,500,"{\"error\":\"oom\"}");return;}
    if(fread(buf,1,sz,f)!=(size_t)sz){fclose(f);free(buf);resp(s,500,"{\"error\":\"read error\"}");return;}
    buf[sz]='\0'; fclose(f);
    const char *mime="text/html";
    if(strstr(fp,".css")) mime="text/css";
    else if(strstr(fp,".js")) mime="application/javascript";
    else if(strstr(fp,".png")) mime="image/png";
    hdr(s,200,mime,(int)sz);
    send(s,buf,(int)sz,0);
    free(buf);
}


/* ── Citizen report handlers ─────────────────────────────── */

/* POST /api/citizen/report
   Body JSON: { bin_id, report_type, description, reporter_name, auto_dispatch }
   report_type: 0=overflow 1=vandalized 2=hazard 3=full
*/
static void h_citizen_report(SOCK_T s, Req *r) {
    if (strcmp(r->method, "POST")) {
        resp(s, 405, "{\"error\":\"POST required\"}"); return;
    }
    int bin_id = 0, report_type = 0, auto_dispatch = 1;
    char description[128] = "", reporter_name[48] = "";

    jint(r->body, "bin_id",       &bin_id);
    jint(r->body, "report_type",  &report_type);
    jint(r->body, "auto_dispatch",&auto_dispatch);
    jstr(r->body, "description",  description,   128);
    jstr(r->body, "reporter_name",reporter_name,  48);

    if (bin_id <= 0) {
        resp(s, 400, "{\"error\":\"bin_id required\"}"); return;
    }
    if (report_type < 0 || report_type > 3) {
        resp(s, 400, "{\"error\":\"report_type must be 0-3 (overflow/vandalized/hazard/full)\"}"); return;
    }

    /* Look up bin to return location name */
    int bi = bin_find(bin_id);
    if (bi < 0) {
        resp(s, 404, "{\"error\":\"Bin not found\"}"); return;
    }

    int rid = citizen_report(bin_id, report_type, description, reporter_name, auto_dispatch);
    if (rid < 0) {
        char eb[128];
        const char *em = (rid==-1)?"report log full":(rid==-3)?"bin not found":"invalid type";
        snprintf(eb,128,"{\"error\":\"%s\"}",em);
        resp(s, 400, eb); return;
    }

    /* Build response with updated bin priority and dispatch info */
    Bin *b = &g_bins[bi];
    int dispatched = (rid > 0 && g_citizen_reports[g_citizen_report_count-1].dispatched);

    /* Find route created for this emergency (most recent emergency route) */
    int emg_route_id = 0; float emg_dist = 0; int emg_vid = 0;
    char emg_route_str[256] = "";
    if (dispatched) {
        for (int i = g_route_count-1; i >= 0; i--) {
            if (g_routes[i].is_emergency) {
                emg_route_id = g_routes[i].route_id;
                emg_dist     = g_routes[i].total_distance;
                emg_vid      = g_routes[i].vehicle_id;
                snprintf(emg_route_str, sizeof(emg_route_str), "%s", g_routes[i].route_string);
                break;
            }
        }
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{"
        "\"success\":true,"
        "\"report_id\":%d,"
        "\"bin_id\":%d,"
        "\"location\":\"%s\","
        "\"reason\":\"%s\","
        "\"citizen_priority\":%d,"
        "\"effective_priority\":%d,"
        "\"dispatched\":%s,"
        "\"emergency_route_id\":%d,"
        "\"emergency_vehicle_id\":%d,"
        "\"emergency_distance_km\":%.2f,"
        "\"route_path\":\"%s\","
        "\"message\":\"%s\""
        "}",
        rid, bin_id, b->location,
        b->last_report_reason,
        b->citizen_priority,
        b->priority,
        dispatched ? "true":"false",
        emg_route_id, emg_vid, emg_dist,
        emg_route_str[0] ? emg_route_str : "pending",
        dispatched
          ? "Emergency vehicle dispatched — route created immediately"
          : "Report recorded — bin flagged for priority collection");
    resp(s, 200, buf);
}

/* GET  /api/citizen/reports?n=50   — list recent reports
   POST /api/citizen/reports        — same via body */
static void h_citizen_reports(SOCK_T s, Req *r) {
    if (strcmp(r->method,"GET") && strcmp(r->method,"POST")) {
        resp(s,405,"{\"error\":\"GET or POST required\"}"); return;
    }
    int n = 50;
    jint(r->body, "n", &n);
    if (n <= 0 || n > MAX_CITIZEN_REPORTS) n = 50;
    resp_free(s, 200, citizen_reports_json(n));
}

/* GET /api/citizen/summary */
static void h_citizen_summary(SOCK_T s, Req *r) {
    (void)r;
    resp_free(s, 200, citizen_summary_json());
}

/* POST /api/citizen/resolve  { report_id } */
static void h_citizen_resolve(SOCK_T s, Req *r) {
    if (strcmp(r->method,"POST")) {
        resp(s,405,"{\"error\":\"POST required\"}"); return;
    }
    int rid = 0;
    jint(r->body, "report_id", &rid);
    if (rid <= 0) { resp(s,400,"{\"error\":\"report_id required\"}"); return; }
    if (citizen_resolve(rid) < 0) {
        resp(s, 404, "{\"error\":\"Report not found\"}"); return;
    }
    resp(s, 200, "{\"success\":true,\"message\":\"Report resolved\"}");
}

/* GET /api/heatmap */
static void h_heatmap(SOCK_T s, Req *r) {
    (void)r;
    resp_free(s, 200, heatmap_json());
}

/* POST /api/auth/login  { username, password } */
static void h_login(SOCK_T s, Req *r) {
    if (strcmp(r->method,"POST")) {
        resp(s,405,"{\"error\":\"POST required\"}"); return;
    }
    char user[32]="", pass[64]="";
    jstr(r->body,"username",user,32);
    jstr(r->body,"password",pass,64);
    if (!user[0] || !pass[0]) {
        resp(s,400,"{\"error\":\"username and password required\"}"); return;
    }
    if (auth_login(user,pass)) {
        resp_free(s,200,auth_session_json());
    } else {
        resp(s,401,"{\"error\":\"Invalid credentials\"}");
    }
}

/* POST /api/auth/logout */
static void h_logout(SOCK_T s, Req *r) {
    (void)r;
    auth_logout();
    resp(s,200,"{\"success\":true,\"message\":\"Logged out\"}");
}

/* GET /api/auth/session */
static void h_session(SOCK_T s, Req *r) {
    (void)r;
    resp_free(s,200,auth_session_json());
}

/* GET /api/sim/steps — animated truck positions */
static void h_sim_steps(SOCK_T s, Req *r) {
    (void)r;
    /* Build JSON array of all recorded TruckStep entries */
    int sz = 64 + g_sim_step_count * 180;
    char *buf = malloc(sz);
    if (!buf) { resp(s,500,"{\"error\":\"oom\"}"); return; }
    int p = snprintf(buf, sz, "[");
    for (int i = 0; i < g_sim_step_count; i++) {
        TruckStep *ts = &g_sim_steps[i];
        p += snprintf(buf+p, sz-p,
            "%s{"
            "\"route_id\":%d,"
            "\"vehicle_id\":%d,"
            "\"plate\":\"%s\","
            "\"lat\":%.6f,\"lon\":%.6f,"
            "\"prev_lat\":%.6f,\"prev_lon\":%.6f,"
            "\"bin_collected\":%d,"
            "\"step_index\":%d,"
            "\"total_steps\":%d,"
            "\"done\":%s"
            "}",
            i ? "," : "",
            ts->route_id, ts->vehicle_id, ts->plate,
            ts->lat, ts->lon, ts->prev_lat, ts->prev_lon,
            ts->bin_collected, ts->step_index, ts->total_steps,
            ts->done ? "true":"false");
        if (p > sz-200) break;
    }
    snprintf(buf+p, sz-p, "]");
    resp_free(s, 200, buf);
}


/* ── Driver leave/return handlers ── */
static void h_driver_leave(SOCK_T s, Req *r) {
    if (strcmp(r->method,"POST")) { resp(s,405,"{\"error\":\"POST required\"}"); return; }
    int did=0; char reason[48]="sick";
    jint(r->body,"driver_id",&did);
    jstr(r->body,"reason",reason,48);
    if (!did) { resp(s,400,"{\"error\":\"driver_id required\"}"); return; }
    if (driver_mark_leave(did,reason)<0) { resp(s,404,"{\"error\":\"Driver not found\"}"); return; }
    char buf[256];
    snprintf(buf,sizeof(buf),
        "{\"success\":true,\"message\":\"Driver %d marked on leave (%s). Substitute auto-assigned if available.\"}",
        did, reason);
    resp(s,200,buf);
}
static void h_driver_return(SOCK_T s, Req *r) {
    if (strcmp(r->method,"POST")) { resp(s,405,"{\"error\":\"POST required\"}"); return; }
    int did=0;
    jint(r->body,"driver_id",&did);
    if (!did) { resp(s,400,"{\"error\":\"driver_id required\"}"); return; }
    if (driver_mark_returned(did)<0) { resp(s,404,"{\"error\":\"Driver not found\"}"); return; }
    resp(s,200,"{\"success\":true,\"message\":\"Driver marked as returned and available\"}");
}
static void h_driver_absences(SOCK_T s, Req *r) {
    (void)r; resp_free(s,200,driver_absence_report_json());
}

/* ── Vehicle breakdown resolve ── */
static void h_vehicle_breakdown_resolve(SOCK_T s, Req *r) {
    if (strcmp(r->method,"POST")) { resp(s,405,"{\"error\":\"POST required\"}"); return; }
    int vid=0;
    jint(r->body,"vehicle_id",&vid);
    if (!vid) { resp(s,400,"{\"error\":\"vehicle_id required\"}"); return; }
    int i=vehicle_find(vid);
    if (i<0) { resp(s,404,"{\"error\":\"Vehicle not found\"}"); return; }
    vehicle_service(vid);
    char buf[256];
    snprintf(buf,sizeof(buf),
        "{\"success\":true,\"message\":\"V%d breakdown cleared — send for refuel to resume service\"}",vid);
    resp(s,200,buf);
}

static void dispatch(SOCK_T s,Req *r){
    if(!strcmp(r->method,"OPTIONS")){resp(s,200,"{}");return;}
    if(!strcmp(r->path,"/api/bins"))           {h_bins(s,r);return;}
    if(!strcmp(r->path,"/api/bins/addwaste"))  {h_add_waste(s,r);return;}
    if(!strcmp(r->path,"/api/vehicles"))       {h_vehicles(s,r);return;}
    if(!strcmp(r->path,"/api/drivers"))        {h_drivers(s,r);return;}
    if(!strcmp(r->path,"/api/routes"))         {h_routes(s,r);return;}
    if(!strcmp(r->path,"/api/simulate"))       {h_simulate(s,r);return;}
    if(!strcmp(r->path,"/api/newday"))         {h_newday(s,r);return;}
    if(!strcmp(r->path,"/api/emergency"))      {h_emergency(s,r);return;}
    if(!strcmp(r->path,"/api/constraints"))    {h_constraints(s);return;}
    if(!strcmp(r->path,"/api/dashboard"))      {h_dashboard(s);return;}
    if(!strcmp(r->path,"/api/facilities"))     {h_facilities(s);return;}
    if(!strcmp(r->path,"/api/log"))            {h_log(s,r);return;}
    if(!strcmp(r->path,"/api/reports/daily"))  {resp_free(s,200,sim_report_daily_json());return;}
    if(!strcmp(r->path,"/api/reports/fuel"))   {resp_free(s,200,sim_report_fuel_json());return;}
    if(!strcmp(r->path,"/api/reports/cost"))   {resp_free(s,200,sim_report_cost_json());return;}
    if(!strcmp(r->path,"/api/reports/zone"))   {resp_free(s,200,sim_report_zone_json());return;}
    if(!strcmp(r->path,"/api/reports/recycling")){resp_free(s,200,sim_report_recycling_json());return;}
    if(!strcmp(r->path,"/api/reports/maintenance")){resp_free(s,200,sim_report_maintenance_json());return;}
    /* ── Citizen reporting ── */
    if(!strcmp(r->path,"/api/citizen/report"))   {h_citizen_report(s,r);return;}
    if(!strcmp(r->path,"/api/citizen/reports"))  {h_citizen_reports(s,r);return;}
    if(!strcmp(r->path,"/api/citizen/summary"))  {h_citizen_summary(s,r);return;}
    if(!strcmp(r->path,"/api/citizen/resolve"))  {h_citizen_resolve(s,r);return;}
    /* ── Driver leave management ── */
    if(!strcmp(r->path,"/api/drivers/leave"))    {h_driver_leave(s,r);return;}
    if(!strcmp(r->path,"/api/drivers/return"))   {h_driver_return(s,r);return;}
    if(!strcmp(r->path,"/api/drivers/absences")) {h_driver_absences(s,r);return;}
    /* ── Vehicle breakdown ── */
    if(!strcmp(r->path,"/api/vehicles/resolve_breakdown")){h_vehicle_breakdown_resolve(s,r);return;}
    /* ── Heatmap ── */
    if(!strcmp(r->path,"/api/heatmap"))           {h_heatmap(s,r);return;}
    /* ── Auth ── */
    if(!strcmp(r->path,"/api/auth/login"))        {h_login(s,r);return;}
    if(!strcmp(r->path,"/api/auth/logout"))       {h_logout(s,r);return;}
    if(!strcmp(r->path,"/api/auth/session"))      {h_session(s,r);return;}
    /* ── Sim steps (truck animation) ── */
    if(!strcmp(r->path,"/api/sim/steps"))         {h_sim_steps(s,r);return;}
    /* Static files */
    const char *p=r->path;
    if(!strcmp(p,"/")) p="/index.html";
    serve_file(s,p);
}

int main(void){
#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2,2),&wd);
#endif
    storage_init("data","logs");
    storage_load_all();
    auth_init();
    citizen_init();

    /* Init day if blank */
    if(!g_today.date[0]){
        time_t now=time(NULL); char d[20];
        strftime(d,sizeof(d),"%Y-%m-%d",localtime(&now));
        strncpy(g_today.date,d,19);g_today.date[19]=0;
    }

    SOCK_T srv=socket(AF_INET,SOCK_STREAM,0);
    int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&opt,sizeof(opt));
    struct sockaddr_in addr={0};
    addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(PORT);
    if(bind(srv,(struct sockaddr*)&addr,sizeof(addr))<0){perror("bind");return 1;}
    listen(srv,10);
    printf("[SWM v3] Running on http://localhost:%d\n",PORT);
    printf("  Bins:%d  Vehicles:%d  Drivers:%d\n",g_bin_count,g_vehicle_count,g_driver_count);
    printf("  POST /api/simulate   → auto-schedule + run all simultaneously\n");
    printf("  POST /api/bins/addwaste → add waste to existing bin in real-time\n");
    printf("  POST /api/newday     → new day reset (infinite simulations)\n\n");

    char buf[RBUF+4096];
    while(1){
        struct sockaddr_in cli; socklen_t clen=sizeof(cli);
        SOCK_T cs=accept(srv,(struct sockaddr*)&cli,&clen);
        if(cs==(SOCK_T)INVALID_SOCKET) continue;
        int n=recv(cs,buf,sizeof(buf)-1,0);
        if(n>0){ buf[n]='\0'; Req req; if(!parse(buf,n,&req)) dispatch(cs,&req); }
        CLOSESOCK(cs);
    }
    return 0;
}
