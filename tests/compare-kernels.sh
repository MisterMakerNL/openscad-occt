#!/usr/bin/env bash
# Multi-kernel differential: run every sample through every backend and compare
# kernel-invariant properties (bbox corners, mesh volume / kernel volume). Meshes
# won't match triangle-for-triangle; the solids must. Requires a build with
# OSCAD_WITH_SMLIB (and optionally OSCAD_WITH_SOLIDCORE for the third column).
#   tests/compare-kernels.sh [files...]       (default: tests/*.scad examples/*.scad)
#
# Third kernel: solidcore has no tessellator, so instead of an STL it reports
# kernel-side bbox + volume through `oscad --kernel solidcore --metrics
# c.json`; the third-column comparison reads that JSON and compares it against
# the occt STL-derived numbers. The occt-vs-smlib behaviour is unchanged.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
oscad="$root/build/oscad"
BTOL=${BTOL:-0.05}    # max bbox corner deviation
VTOL=${VTOL:-0.01}    # max relative volume deviation (tessellation chord error)
files=("$@"); [ ${#files[@]} -eq 0 ] && files=("$root"/tests/*.scad "$root"/examples/*.scad)
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0; unsup=0; scpass=0; scfail=0; scunsup=0
printf "%-22s %-10s %-10s %-8s %-10s %-8s %s\n" file bbox_dmax vol_occt vol_rel sc_bbox_dmax sc_vol_rel result
for f in "${files[@]}"; do
  name="$(basename "$f" .scad)"
  if ! timeout 60 "$oscad" "$f" --kernel occt -o "$tmp/a.stl" --deflection 0.01 >/dev/null 2>"$tmp/a.err"; then
    printf "%-22s %-10s %-10s %-8s %-10s %-8s %s\n" "$name" - - - - - "SKIP (occt: $(head -c 60 "$tmp/a.err"))"; continue
  fi
  # --- occt vs smlib (unchanged two-kernel behaviour) ---
  smlib_result="-"; d="-"; va="-"; rel="-"
  if ! timeout 60 "$oscad" "$f" --kernel smlib -o "$tmp/b.stl" --deflection 0.01 >/dev/null 2>"$tmp/b.err"; then
    smlib_result="UNSUP ($(head -c 70 "$tmp/b.err" | tr '\n' ' '))"; unsup=$((unsup+1))
  else
    d=$(python3 "$root/tests/bbox.py" diff "$tmp/a.stl" "$tmp/b.stl")
    va=$(python3 "$root/tests/bbox.py" vol "$tmp/a.stl"); vb=$(python3 "$root/tests/bbox.py" vol "$tmp/b.stl")
    rel=$(python3 -c "import sys; a,b=float(sys.argv[1]),float(sys.argv[2]); print('%.4f'%(abs(a-b)/abs(a) if a else abs(b)))" "$va" "$vb")
    ok=$(python3 -c "import sys; print(int(float(sys.argv[1])<=$BTOL and float(sys.argv[2])<=$VTOL))" "$d" "$rel")
    if [ "$ok" = 1 ]; then smlib_result=ok; pass=$((pass+1)); else smlib_result=MISMATCH; fail=$((fail+1)); fi
  fi
  # --- occt vs solidcore (kernel metrics: bbox + volume; no tessellation) ---
  sc_result="-"; sc_cols="- -"
  if timeout 60 "$oscad" "$f" --kernel solidcore --metrics "$tmp/c.json" >/dev/null 2>"$tmp/c.err"; then
    read -r dsc relsc < <(python3 - "$tmp/a.stl" "$tmp/c.json" <<'EOF'
import json, struct, sys
def stl_bbox(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[80:84])[0]
    o = 84
    mn = [9e9]*3; mx = [-9e9]*3
    for _ in range(n):
        v = struct.unpack("<12f", d[o:o+48]); o += 50
        for k in range(3):
            for c in range(3):
                x = v[3+k*3+c]; mn[c] = min(mn[c], x); mx[c] = max(mx[c], x)
    return mn, mx
def stl_vol(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[80:84])[0]; o = 84; vol = 0.0
    for _ in range(n):
        v = struct.unpack("<12f", d[o:o+48]); o += 50
        ax, ay, az = v[3:6]; bx, by, bz = v[6:9]; cx, cy, cz = v[9:12]
        vol += ax*(by*cz-bz*cy) - ay*(bx*cz-bz*cx) + az*(bx*cy-by*cx)
    return vol/6.0
m = json.load(open(sys.argv[2]))
bmn, bmx = stl_bbox(sys.argv[1])
d = max(abs(s-j) for s, j in zip(bmn+bmx, m["bbox"]))
va = stl_vol(sys.argv[1]); vb = m["volume"]
rel = abs(va-vb)/abs(va) if va else abs(vb)
print(f"{d:.6f} {rel:.4f}")
EOF
)
    sc_cols="$dsc $relsc"
    ok=$(python3 -c "import sys; print(int(float(sys.argv[1])<=$BTOL and float(sys.argv[2])<=$VTOL))" "$dsc" "$relsc")
    if [ "$ok" = 1 ]; then sc_result=ok; scpass=$((scpass+1)); else sc_result=SC-MISMATCH; scfail=$((scfail+1)); fi
  else
    sc_result="SC-UNSUP ($(head -c 70 "$tmp/c.err" | tr '\n' ' '))"; scunsup=$((scunsup+1))
  fi
  printf "%-22s %-10s %-10s %-8s %-10s %-8s %s\n" "$name" "$d" "$va" "$rel" "$sc_cols" "$sc_result" "smlib: $smlib_result; solidcore: $sc_result"
done
echo "----"
echo "occt-vs-smlib: agree=$pass mismatch=$fail unsupported_by_smlib=$unsup"
echo "occt-vs-solidcore: agree=$scpass mismatch=$scfail unsupported_by_solidcore=$scunsup"
[ "$fail" -eq 0 ] && [ "$scfail" -eq 0 ]
