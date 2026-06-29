#include "routes.h"
#ifndef SIMULATION_H
#define SIMULATION_H
#include "types.h"

typedef struct {
    int   success;
    int   routes_run;
    int   bins_collected;
    int   disposal_trips;
    float total_distance;
    float fuel_used;
    float fuel_cost;
    float labor_cost;
    float total_cost;
    char  message[512];
} SimResult;

/* Run a full day simulation — NEVER exhausts:
   calls new_day_reset automatically if needed */
SimResult sim_run(float fuel_price);

/* New day: refill bins, refuel vehicles, rest drivers, clear routes */
void sim_new_day(void);

/* Emergency dispatch + immediate simulate */
EmergencyResult sim_emergency(int bin_id, const char *reason);

/* Add waste to a bin in real-time */
int sim_add_waste(int bin_id, float kg);

/* Dashboard JSON */
char *sim_dashboard_json(void);

/* Reports */
char *sim_report_daily_json(void);
char *sim_report_fuel_json(void);
char *sim_report_cost_json(void);
char *sim_report_zone_json(void);
char *sim_report_recycling_json(void);
char *sim_report_maintenance_json(void);

/* Log */
char *sim_log_json(int last_n);
void  sim_log(const char *level, const char *module, const char *fmt, ...);

#endif
