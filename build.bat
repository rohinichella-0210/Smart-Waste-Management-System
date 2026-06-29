@echo off
echo Building Smart Waste Management System v3...
gcc -Wall -O2 -Isrc ^
  src/server.c src/bins.c src/vehicles.c src/drivers.c ^
  src/routes.c src/simulation.c src/storage.c src/validation.c ^
  -lm -lws2_32 -o smart_waste.exe
if %errorlevel%==0 (
  echo.
  echo [SUCCESS] Build complete!
  echo Run: smart_waste.exe
  echo Then open: http://localhost:8080
) else (
  echo.
  echo [FAILED] Check errors above.
)
