#ifndef VEHICLES_H
#define VEHICLES_H
#include "types.h"

int   vehicles_init(void);
int   vehicle_add(Vehicle *v);
int   vehicle_find(int id);
int   vehicle_find_best(const char *zone, const char *waste_type,
                        float waste_kg, float route_km);
int   vehicle_refuel(int vid, float litres);
int   vehicle_delete(int vid);
int   vehicle_service(int vid);   /* perform maintenance, reset flag */
void  vehicle_reset_daily(void);  /* new day: refuel, reset odometer, clear assignments */
char *vehicles_to_json(void);

/* Called during simulation */
void  vehicle_deduct_fuel(int vid, float km);
void  vehicle_add_load(int vid, float kg);
void  vehicle_unload(int vid);    /* waste dumped at disposal site */
void  vehicle_set_status(int vid, const char *status);

#endif
