#ifndef TYPES_H
#define TYPES_H

/* ── Limits ── */
#define MAX_BINS        300
#define MAX_VEHICLES    128
#define MAX_DRIVERS     256
#define MAX_ROUTES      512
#define MAX_BINS_PER_RT  64
#define MAX_LOG_ENTRIES 2000
#define MAX_DISPOSAL     16
#define MAX_USERS        16

/* ── Carbon emission factor kg CO2 per litre diesel ── */
#define CO2_PER_LITRE   2.68f

/* ── Roles ── */
#define ROLE_ADMIN    0
#define ROLE_OPERATOR 1

/* ── Facilities (Depot, Disposal Sites, Recycling Plants) ── */
#define FACILITY_GARAGE   0   /* main garage / start point */
#define FACILITY_DISPOSAL 1   /* general waste dump */
#define FACILITY_RECYCLE  2   /* recycling plant */
#define FACILITY_HAZARD   3   /* hazardous waste facility */
#define NUM_FACILITIES    4

typedef struct {
    int   id;
    char  name[64];
    int   type;           /* FACILITY_* */
    float lat, lon;
    float capacity_kg;    /* 0 = unlimited */
    float current_load;
} Facility;

extern Facility g_facilities[NUM_FACILITIES];

/* ── Waste Types ── */
#define WASTE_DRY       "dry"
#define WASTE_WET       "wet"
#define WASTE_HAZARDOUS "hazardous"
#define WASTE_MIXED     "mixed"

/* ── Bin ── */
typedef struct {
    int   bin_id;
    char  location[64];
    float fill_level;     /* 0-100 % */
    float capacity_kg;
    char  waste_type[16]; /* dry / wet / hazardous / mixed */
    int   threshold;
    char  zone[16];
    int   collected_today;
    float lat, lon;
    int   priority;       /* 0=normal 1=high 2=critical 3=overflow */
    int   priority_order; /* numeric rank used for collection order: 1=first, 2=second, ... */
    float predicted_fill;
    float fill_rate;      /* % per day */
    char  last_collected[20];
    float total_waste_collected; /* cumulative kg */
    /* ── Citizen reporting ── */
    int   citizen_report_count;   /* total reports ever submitted */
    int   citizen_priority;       /* 0=none 1=moderate 2=urgent (overflow/vandal) */
    int   is_vandalized;          /* 1 = bin marked vandalized by citizens */
    char  last_report_reason[32]; /* e.g. "overflow", "vandalized", "hazard" */
    char  last_report_time[20];
} Bin;

extern Bin  g_bins[];
extern int  g_bin_count;

/* ── Vehicle ── */
typedef struct {
    int   vehicle_id;
    char  plate[20];
    char  vehicle_class[16]; /* normal / special / hazardous */
    float capacity_kg;
    float current_load;
    float fuel_level;
    float fuel_capacity;
    float fuel_rate;      /* L/km */
    char  zone[16];
    int   available;
    int   assigned_route;
    int   driver_id;
    float total_distance; /* daily odometer */
    float lifetime_km;
    int   needs_maintenance;
    int   maintenance_due_km;
    char  status[32];
    float current_lat, current_lon;
    /* NEW */
    float co2_kg;          /* cumulative CO2 this vehicle emitted today */
    int   hazard_capable;  /* 1 = can carry hazardous waste */
} Vehicle;

extern Vehicle g_vehicles[];
extern int     g_vehicle_count;

/* ── Driver ── */
typedef struct {
    int   driver_id;
    char  name[64];
    char  license[32];
    char  shift[16];      /* morning / evening */
    int   is_substitute;
    int   vehicle_id;     /* 0 = unassigned */
    float hours_worked;
    float max_hours;
    float salary_per_hour;
    float overtime_hours;
    int   available;
    /* ── Absence management ── */
    int   on_leave;           /* 1 = marked absent/on-leave today */
    char  leave_reason[48];   /* e.g. "sick", "personal", "holiday" */
    int   absent_days;        /* cumulative absent days counter */
} Driver;

extern Driver g_drivers[];
extern int    g_driver_count;

/* ── Route (with full lifecycle: garage→bins→disposal→garage) ── */
typedef struct {
    int   route_id;
    int   vehicle_id;
    char  zone[16];
    int   bin_sequence[MAX_BINS_PER_RT];
    int   bin_count;

    /* Distances (km) */
    float dist_garage_to_first;
    float dist_bins;
    float dist_to_disposal;
    float dist_disposal_to_garage;
    float total_distance;

    float fuel_required;
    float total_waste_kg;
    float fuel_cost;
    float labor_cost;
    float total_cost;

    int   disposal_facility_id;
    char  disposal_name[64];

    int   completed;
    int   is_emergency;
    int   mid_trip_disposal;

    char  route_string[1024];
    char  created_at[20];
    char  completed_at[20];

    /* NEW */
    float co2_kg;          /* CO2 for this route */
    int   has_hazardous;   /* 1 = route carries hazardous bins */
} Route;

extern Route g_routes[];
extern int   g_route_count;

/* ── Daily Log Entry ── */
typedef struct {
    char  timestamp[20];
    char  level[8];       /* INFO / WARN / ERROR / EVENT */
    char  module[16];     /* SIM / ROUTE / VEHICLE / DRIVER / EMERG */
    char  message[256];
} LogEntry;

extern LogEntry g_log[];
extern int      g_log_count;

/* ── Daily Stats ── */
typedef struct {
    char  date[20];
    int   simulations_run;
    int   bins_collected;
    int   routes_completed;
    float total_distance_km;
    float total_fuel_used;
    float total_fuel_cost;
    float total_labor_cost;
    float total_cost;
    int   emergency_count;
    int   maintenance_events;
    float recycling_kg;
    float hazardous_kg;
    float general_kg;
    float organic_kg;
    float dry_kg;         /* NEW waste types */
    float wet_kg;
    float mixed_kg;
    float recyclable_kg;  /* Recycling & Waste Processing Module */
    float non_recyclable_kg;
    float landfill_kg;
    float co2_total_kg;          /* NEW */
    int   hazard_collections;    /* NEW */
} DailyStats;

extern DailyStats g_today;
extern DailyStats g_history[365];
extern int        g_history_count;

/* ── User / Login ── */
typedef struct {
    char username[32];
    char password[64];
    int  role;   /* ROLE_ADMIN / ROLE_OPERATOR */
    int  active;
} User;

extern User g_users[MAX_USERS];
extern int  g_user_count;

/* ── Session state ── */
extern int  g_logged_in;
extern int  g_current_role;
extern char g_current_user[32];

/* ── Zone Heatmap ── */
typedef struct {
    char  zone[16];
    int   overflow_count;
    float avg_fill_rate;
    float zone_score;
    char  heat_level[8];  /* green / yellow / red */
} ZoneHeat;

extern ZoneHeat g_zone_heat[64];
extern int      g_zone_heat_count;

/* ── Simulation steps (for animated truck movement API) ── */
typedef struct {
    int   route_id;
    int   vehicle_id;
    char  plate[20];
    float lat, lon;         /* current truck position */
    float prev_lat, prev_lon;
    int   bin_collected;    /* bin_id just collected (0=none) */
    int   step_index;
    int   total_steps;
    int   done;             /* 1 = this truck finished */
} TruckStep;

#define MAX_TRUCK_STEPS 4096
extern TruckStep g_sim_steps[MAX_TRUCK_STEPS];
extern int       g_sim_step_count;

/* ── Citizen Report ── */
#define REPORT_TYPE_OVERFLOW   0
#define REPORT_TYPE_VANDALIZED 1
#define REPORT_TYPE_HAZARD     2
#define REPORT_TYPE_FULL       3
#define MAX_CITIZEN_REPORTS    500

typedef struct {
    int   report_id;
    int   bin_id;
    int   report_type;        /* REPORT_TYPE_* */
    char  reason[32];         /* "overflow","vandalized","hazard","full" */
    char  description[128];   /* optional text from citizen */
    char  reporter_name[48];  /* optional, defaults to "Anonymous" */
    char  timestamp[20];
    int   dispatched;         /* 1 = emergency dispatch triggered */
    int   resolved;           /* 1 = collected/resolved */
    float weight;             /* citizen_priority weight contributed */
} CitizenReport;

extern CitizenReport g_citizen_reports[MAX_CITIZEN_REPORTS];
extern int           g_citizen_report_count;
extern int           g_citizen_report_next_id;

#endif
