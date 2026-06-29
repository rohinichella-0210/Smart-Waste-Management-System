#ifndef DRIVERS_H
#define DRIVERS_H
#include "types.h"

int   drivers_init(void);
int   driver_add(Driver *d);
int   driver_find(int id);
int   driver_find_for_vehicle(int vehicle_id);
int   driver_find_substitute(const char *shift);
int   driver_delete(int did);
void  driver_reset_daily(void);   /* new day: reset hours, restore availability */
char *drivers_to_json(void);

void  driver_log_hours(int did, float hours);
int   driver_assign_vehicle(int did, int vid);

/* Shift scheduling: distribute morning/evening/night equally */
void  drivers_assign_shifts(void);

/* ── Absence / Leave management ── */
int   driver_mark_leave(int driver_id, const char *reason);
int   driver_mark_returned(int driver_id);
char *driver_absence_report_json(void);
const char *driver_current_shift(void);

#endif
