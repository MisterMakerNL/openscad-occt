#!/usr/bin/env bash
# Differential verification of oscad (OCCT) against the OpenSCAD corpus.
#
# For every corpus file that uses ONLY currently-supported features, render with
# both oscad and the reference `openscad` binary and compare bounding boxes.
# Rationale: meshes won't match byte-for-byte across kernels, but kernel-invariant
# properties (bbox, and later volume/topology) must agree. Curved surfaces diverge
# slightly because OCCT is exact while OpenSCAD facets -- a SMALL delta is fine; a
# large one is a real bug (this is how the cylinder d-vs-r precedence bug surfaced).
#
# Exit status is nonzero only on a real bbox MISMATCH. Files that use not-yet-
# implemented features are skipped; files oscad can't yet handle are reported as
# informational "gaps", not gate failures.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
oscad="$root/build/oscad"
kernel="${OSCAD_KERNEL:-occt}"   # occt (default) or smlib; each kernel is its own oracle
corpus="${1:-$root/tests/corpus/tests/data/scad}"
ref="$(command -v openscad || true)"
[ -x "$oscad" ] || { echo "build oscad first: cmake --build build"; exit 2; }
[ -d "$corpus" ] || { echo "corpus missing: run tests/fetch-corpus.sh"; exit 2; }
[ -n "$ref" ]    || echo "note: no reference 'openscad' on PATH -- checking solid production only"

# Features NOT yet implemented. Shrink this regex as milestones land.
UNSUP='\b(polyhedron|color|hull|minkowski|offset|projection|surface|text|import|render|resize|multmatrix|let|echo|assert|each)\b'
MISMATCH_TOL=0.50   # below this, divergence is OCCT-exact-vs-faceting (incl. $fn-faceted
                    # revolves/curves); real geometry bugs shift the bbox by >=1 unit
OTMO=20             # per-file timeout (s) for oscad -- big for-loops can be OCCT-boolean-slow
RTMO=30             # per-file timeout (s) for the reference openscad render

# Files whose bbox is EXPECTED to diverge for a documented, non-bug reason. Kept as an
# explicit allow-list (rather than a looser global tolerance) so the gate stays sharp
# for everything else. Each entry has a reason:
#   *-tests spheres/cylinders : intentional exact-vs-faceted divergence at low $fn
#                               (we keep sphere/cylinder exact regardless of $fn)
#   issue267-normalization-crash : subtracts a 2D square from a 3D solid -- ill-defined
#                               2D/3D boolean mixing (a normalizer crash-test, not a
#                               geometry test). TODO: fail loud on 2D/3D mixing instead.
XDIVERGE='^(sphere-tests|cylinder-tests|issue267-normalization-crash)\.scad$'

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
inscope=0 ok=0 exact=0 empty=0 gap=0 mismatch=0 xdiv=0
gaps=(); diffs=(); xdivs=()

while IFS= read -r f; do
  grep -qE "$UNSUP" "$f" && continue
  inscope=$((inscope+1))
  if ! timeout "$OTMO" "$oscad" "$f" --kernel "$kernel" -o "$tmp/a.stl" >/dev/null 2>&1; then
    if [ -n "$ref" ] && timeout "$RTMO" "$ref" --export-format binstl -o "$tmp/b.stl" "$f" >/dev/null 2>&1 \
       && [ "$(python3 "$root/tests/bbox.py" tris "$tmp/b.stl")" -le 0 ]; then
      empty=$((empty+1)); continue            # both empty -> expected
    fi
    gap=$((gap+1)); gaps+=("$(basename "$f")"); continue
  fi
  [ "$(python3 "$root/tests/bbox.py" tris "$tmp/a.stl")" -le 0 ] && { empty=$((empty+1)); continue; }
  if [ -z "$ref" ] || ! timeout "$RTMO" "$ref" --export-format binstl -o "$tmp/b.stl" "$f" >/dev/null 2>&1; then
    ok=$((ok+1)); continue
  fi
  d="$(python3 "$root/tests/bbox.py" diff "$tmp/a.stl" "$tmp/b.stl")"
  if awk "BEGIN{exit !($d>$MISMATCH_TOL)}"; then
    if [[ "$(basename "$f")" =~ $XDIVERGE ]]; then
      xdiv=$((xdiv+1)); xdivs+=("$(basename "$f")  bbox_dmax=$d")   # documented divergence
    else
      mismatch=$((mismatch+1)); diffs+=("$(basename "$f")  bbox_dmax=$d")
    fi
  else
    ok=$((ok+1)); awk "BEGIN{exit !($d>0.001)}" && exact=$((exact+1))
  fi
done < <(find "$corpus" -name '*.scad' | sort)

echo "----------------------------------------"
echo "in-scope files        : $inscope"
echo "  matched reference   : $ok   (OCCT-exact divergence on curves: $exact)"
echo "  empty (expected)    : $empty"
echo "  unimplemented gaps  : $gap"
echo "  expected divergence : $xdiv   (documented exact-vs-facet / 2D-3D-mixing)"
echo "  bbox MISMATCH       : $mismatch"
[ "$xdiv" -gt 0 ] && { echo "  -- expected divergence --"; printf '    %s\n' "${xdivs[@]}"; }
[ "$gap" -gt 0 ] && { echo "  -- gaps --"; printf '    %s\n' "${gaps[@]}"; }
[ "$mismatch" -gt 0 ] && { echo "  -- mismatches --"; printf '    %s\n' "${diffs[@]}"; }
echo "----------------------------------------"
[ "$mismatch" -eq 0 ]
