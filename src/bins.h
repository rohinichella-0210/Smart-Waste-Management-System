#ifndef BINS_H
#define BINS_H
#include "types.h"

int   bins_init(void);
int   bin_add(Bin *b);
int   bin_find(int id);
void  bin_update_priority(int idx);
void  bins_update_all_priorities(void);
void  bins_assign_priority_orders(void); /* compute priority_order for all bins */
int   waste_type_valid(const char *wt);  /* validate waste type string */
int   bin_update_fill(int bin_id, float delta_kg); /* add waste to existing bin */
int   bin_set_fill(int bin_id, float new_pct);
int   bin_delete(int bin_id);
char *bins_to_json(void);
void  bins_simulate_daily_fill(void); /* overnight fill increase */
void  bins_reset_collected(void);     /* new day: clear collected_today */

#endif
