#!/usr/bin/env bash
# Two-kernel differential: run every sample through both backends and compare
# kernel-invariant properties (bbox corners, mesh volume). Meshes won't match
# triangle-for-triangle; the solids must. Requires a build with OSCAD_WITH_SMLIB.
#   tests/compare-kernels.sh [files...]       (default: tests/*.scad examples/*.scad)
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
oscad="$root/build/oscad"
BTOL=${BTOL:-0.05}    # max bbox corner deviation
VTOL=${VTOL:-0.01}    # max relative volume deviation (tessellation chord error)
files=("$@"); [ ${#files[@]} -eq 0 ] && files=("$root"/tests/*.scad "$root"/examples/*.scad)
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0; unsup=0
printf "%-22s %-10s %-10s %-8s %s\n" file bbox_dmax vol_occt vol_rel result
for f in "${files[@]}"; do
  name="$(basename "$f" .scad)"
  if ! timeout 60 "$oscad" "$f" --kernel occt -o "$tmp/a.stl" --deflection 0.01 >/dev/null 2>"$tmp/a.err"; then
    printf "%-22s %-10s %-10s %-8s %s\n" "$name" - - - "SKIP (occt: $(head -c 60 "$tmp/a.err"))"; continue
  fi
  if ! timeout 60 "$oscad" "$f" --kernel smlib -o "$tmp/b.stl" --deflection 0.01 >/dev/null 2>"$tmp/b.err"; then
    printf "%-22s %-10s %-10s %-8s %s\n" "$name" - - - "UNSUP ($(head -c 70 "$tmp/b.err" | tr '\n' ' '))"; unsup=$((unsup+1)); continue
  fi
  d=$(python3 "$root/tests/bbox.py" diff "$tmp/a.stl" "$tmp/b.stl")
  va=$(python3 "$root/tests/bbox.py" vol "$tmp/a.stl"); vb=$(python3 "$root/tests/bbox.py" vol "$tmp/b.stl")
  rel=$(python3 -c "import sys; a,b=float(sys.argv[1]),float(sys.argv[2]); print('%.4f'%(abs(a-b)/abs(a) if a else abs(b)))" "$va" "$vb")
  ok=$(python3 -c "import sys; print(int(float(sys.argv[1])<=$BTOL and float(sys.argv[2])<=$VTOL))" "$d" "$rel")
  if [ "$ok" = 1 ]; then r=ok; pass=$((pass+1)); else r=MISMATCH; fail=$((fail+1)); fi
  printf "%-22s %-10s %-10s %-8s %s\n" "$name" "$d" "$va" "$rel" "$r"
done
echo "----"; echo "agree=$pass mismatch=$fail unsupported_by_smlib=$unsup"
[ "$fail" -eq 0 ]
