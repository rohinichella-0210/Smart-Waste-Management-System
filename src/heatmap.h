#ifndef HEATMAP_H
#define HEATMAP_H
#include "types.h"

/* Rebuild zone scores from current bins + history */
void  heatmap_update(void);

/* Return JSON array of zone heat data */
char *heatmap_json(void);

/* Called when a bin overflows — increments zone overflow counter */
void  heatmap_record_overflow(const char *zone);

#endif
