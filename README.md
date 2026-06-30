# Smart Waste Management System

A C-based system for smart waste collection, route optimization, and live monitoring — built for Chennai Municipal Corporation.

---

## Overview

This system manages the full waste collection lifecycle: tracking bin fill levels, allocating vehicles and drivers, optimizing collection routes, simulating live operations, and surfacing everything through a web dashboard — all without relying on external libraries or services.

## Tech Stack

| Layer | Technology |
|---|---|
| Backend logic | C |
| Networking | Built-in HTTP server |
| Frontend | HTML, CSS, JavaScript |
| Storage | File-based persistence |
| Visualization | Custom heatmap engine |

## Module Breakdown

| File | Responsibility |
|---|---|
| `server.c` | HTTP server and routing |
| `bins.c` | Bin fill-level monitoring |
| `vehicles.c` | Vehicle allocation and tracking |
| `drivers.c` | Driver scheduling and assignment |
| `routes.c` | Route optimization |
| `simulation.c` | Live collection cycle simulation |
| `storage.c` | Data persistence layer |
| `validation.c` | Input validation |
| `auth.c` | Login and session handling |
| `heatmap.c` | Waste density heatmap generation |
| `citizen.c` | Citizen complaint and status reporting |

## System Flow

```
Bin Monitoring
     |
     v
Vehicle & Driver Allocation
     |
     v
Route Optimization
     |
     v
Live Simulation
     |
     v
Heatmap & Reports
     ^
     |
Citizen Reports / Emergency Alerts
```

## Highlights

- Live bin fill tracking with overflow/emergency alerts
- Vehicle and driver allocation driven by real-time bin status
- Route optimization to cut travel time and fuel cost
- Simulation engine for end-to-end collection cycles
- Heatmap view of waste density and collection hotspots
- Citizen-facing portal for issue reporting
- Role-based login for Admin and Operator users
- Self-contained HTTP server — runs with zero external dependencies

## Dashboard

The web dashboard provides live bin and vehicle tracking, route visualization, emergency alerts, heatmap analytics, and a citizen complaint log — all served locally.

## Setup

**1. Build**

Using the provided script:
```
build_windows.bat
```

Or with make:
```
make
```

**2. Start the server**
```
smart_waste.exe
```

**3. Open the dashboard**
```
http://localhost:8080
```

**Login credentials**
```
Admin     -> admin / admin123
Operator  -> operator / op123
```

## Intended Use Cases

- Municipal corporations and urban local bodies
- Smart city waste management initiatives
- Private waste collection operators

## Roadmap

- IoT sensor integration for live bin-level sensing
- ML-based predictive collection scheduling
- Mobile app for drivers and citizens
- Cloud-hosted deployment

## License

For academic purposes only
