#!/bin/sh
# Shared legacy cubing pipeline. Keep diagnostics/results on stdout as before.
set -u
backend=$1
shift
if [ "$#" -lt 1 ]; then
  echo "usage: cube-$backend.sh input.cnf [march options]" >&2
  exit 2
fi
cnf=$1
shift
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cnc-legacy.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$root/march_cu/march_cu" "$cnf" "$@" -o "$tmp/cubes" > "$tmp/march.log"
status=$?
cat "$tmp/march.log"
case "$status" in
  10) exit 0 ;;
  20) echo 's UNSATISFIABLE'; exit 0 ;;
  0) ;;
  *) exit "$status" ;;
esac
if [ ! -f "$tmp/cubes" ]; then
  echo 'cubing produced no cube file' >&2
  exit 2
fi
if [ ! -s "$tmp/cubes" ]; then
  echo 's UNSATISFIABLE'
  exit 0
fi
{
  echo 'p inccnf'
  awk '!/^[[:space:]]*[cp]([[:space:]]|$)/' "$cnf"
  cat "$tmp/cubes"
} > "$tmp/formula.icnf"
case "$backend" in
  glucose) "$root/iglucose/core/iglucose" "$tmp/formula.icnf" -verb=0 ;;
  lingeling) "$root/lingeling/ilingeling" "$tmp/formula.icnf" 8 ;;
  *) echo 'unknown conquer backend' >&2; exit 2 ;;
esac
status=$?
# The old scripts ended with successful cleanup rather than the solver's 10/20.
case "$status" in 0|10|20) exit 0 ;; *) exit "$status" ;; esac
