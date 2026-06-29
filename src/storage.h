#ifndef STORAGE_H
#define STORAGE_H

void storage_init(const char *data_dir, const char *log_dir);
void storage_load_all(void);
void storage_save_bins(void);
void storage_save_vehicles(void);
void storage_save_drivers(void);
void storage_save_routes(void);
void storage_save_stats(void);
void storage_save_log(void);
void storage_save_all(void);

#endif
