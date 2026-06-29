@echo off
echo Building Smart Waste Management System...

gcc -Wall -O2 -Isrc ^
  src/server.c ^
  src/bins.c ^
  src/vehicles.c ^
  src/drivers.c ^
  src/routes.c ^
  src/simulation.c ^
  src/storage.c ^
  src/validation.c ^
  src/auth.c ^
  src/heatmap.c ^
  src/citizen.c ^
  -o smart_waste.exe ^
  -lws2_32 -lm

if %errorlevel% == 0 (
    echo.
    echo BUILD SUCCESS — smart_waste.exe created
    echo.
    echo Run:  smart_waste.exe
    echo Open: http://localhost:8080
    echo.
    echo Default login:
    echo   Admin:    admin / admin123
    echo   Operator: operator / op123
) else (
    echo.
    echo BUILD FAILED — check errors above
    echo Make sure GCC is installed: https://winlibs.com
)
