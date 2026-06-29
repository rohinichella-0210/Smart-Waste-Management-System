#ifndef CITIZEN_H
#define CITIZEN_H
#include "types.h"

/*
 * citizen.h — Citizen Complaint / Report Module
 *
 * Public-facing endpoint that lets residents flag bins.
 * Each report:
 *   1. Logs a CitizenReport record
 *   2. Increments bin.citizen_report_count
 *   3. Raises bin.citizen_priority based on type + count
 *   4. Recomputes bin.priority (effective priority = max of fill-based + citizen)
 *   5. Triggers emergency dispatch if threshold crossed
 */

/* Submit a citizen report. Returns report_id on success, <0 on error.
   auto_dispatch=1 → calls sim_emergency if urgent */
int   citizen_report(int bin_id,
                     int report_type,          /* REPORT_TYPE_* */
                     const char *description,
                     const char *reporter_name,
                     int auto_dispatch);

/* Mark a report resolved (bin collected) */
int   citizen_resolve(int report_id);

/* Recompute citizen_priority for a bin from its open reports */
void  citizen_recompute_priority(int bin_idx);

/* JSON output */
char *citizen_reports_json(int last_n);          /* all or last N */
char *citizen_summary_json(void);                /* stats summary */
char *citizen_bin_reports_json(int bin_id);      /* reports for one bin */

/* Persist */
void  citizen_save(void);
void  citizen_load(void);
void  citizen_init(void);

#endif
