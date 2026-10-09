#!/bin/sh
# Optional orchestration build; the legacy default path remains below.
if [ "$1" = "orchestrator" ]; then
  set -eu
  cd "$(dirname "$0")"
  make -C march_cu
  make -C iglucose/core
  (cd lingeling && ./configure.sh && make ilingeling)
  make -C orchestrator
  exit 0
fi
cd march_cu; make $1; cd ..;
cd iglucose/core; make $1; cd ../simp; make $1; cd ../..;
cd lingeling; ./configure.sh; make $1; cd ..;
