#ifndef ROUTES_H
#define ROUTES_H
#include "types.h"

/* Haversine GPS distance */
float route_haversine(float lat1, float lon1, float lat2, float lon2);

/* Find nearest disposal facility for a given waste_type */
int   route_nearest_disposal(const char *waste_type, float from_lat, float from_lon);

/* Full auto-schedule: finds critical bins, assigns vehicles,
   optimises with NN+2-opt including garage↔disposal legs */
typedef struct {
    int   success;
    int   routes_created;
    int   bins_scheduled;
    float total_distance;
    float total_fuel;
    float total_cost;
    char  message[512];
} ScheduleResult;

ScheduleResult routes_auto_schedule(float fuel_price);

/* Emergency: dispatch nearest suitable vehicle to one bin */
typedef struct {
    int   success;
    int   route_id;
    int   vehicle_id;
    float distance;
    char  message[256];
} EmergencyResult;

EmergencyResult routes_emergency(int bin_id, const char *reason);

char *routes_to_json(void);
int   route_find(int id);
void  routes_clear_completed(void);
void  routes_reset_daily(void);

#endif
