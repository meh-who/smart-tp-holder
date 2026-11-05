#!/bin/sh
# Build and run the sync simulation on a laptop (needs g++ and the ArduinoJson library).
#   ./run.sh               all scenarios
#   ./run.sh fresh log     one scenario, printing every heartbeat
set -e
cd "$(dirname "$0")"
AJ="${ARDUINOJSON:-}"
for d in "$AJ" "$HOME/Documents/Arduino/libraries/ArduinoJson/src" "$HOME/Arduino/libraries/ArduinoJson/src"; do
  [ -n "$d" ] && [ -f "$d/ArduinoJson.h" ] && AJ="$d" && break
done
[ -f "$AJ/ArduinoJson.h" ] || { echo "ArduinoJson not found: set ARDUINOJSON=/path/to/ArduinoJson/src"; exit 1; }
mkdir -p build
cp ../../holder/secrets.example.h build/secrets.h      # dummy credentials; the real secrets.h is never read
g++ -std=c++17 -O1 -Wall -Wno-unused-function -DARDUINO=300 -DARDUINOJSON_ENABLE_PROGMEM=0 \
    -Imock -Ibuild -I"$AJ" -I../../holder sync_sim.cpp -o build/sync_sim
if [ -n "$1" ]; then build/sync_sim "$@"; else for s in fresh steady closed-idle flaky rebuild fragmented outage; do build/sync_sim $s | grep -A4 '^=='; done; fi
