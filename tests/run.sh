#!/usr/bin/env bash
# Build oscad and run it over the sample .scad files, sanity-checking outputs.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
cmake -B build -S . >/dev/null
cmake --build build >/dev/null
out="build/test-out"
mkdir -p "$out"
fail=0
for f in tests/*.scad; do
  name="$(basename "$f" .scad)"
  if ./build/oscad "$f" --kernel "${OSCAD_KERNEL:-occt}" -o "$out/$name.stl" --step "$out/$name.step" >/dev/null 2>&1; then
    sz=$(stat -c%s "$out/$name.stl")
    if [ "$sz" -gt 84 ]; then echo "ok   $name (stl=${sz}B)"; else echo "FAIL $name (empty stl)"; fail=1; fi
  else
    echo "FAIL $name (oscad error)"; fail=1
  fi
done
exit $fail
