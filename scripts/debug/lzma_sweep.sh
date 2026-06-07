#!/usr/bin/env bash
# lzma_sweep.sh — measure the compressed size of the RAM-app image under a matrix of
# xz raw-LZMA1 parameter sets and BCJ filters, to pick the smallest for the stub.
#
# WHY: "Free space" = 40960 - stub_code - compressed_app. The compressed size depends
# only on the xz encoder settings (NOT the stub's decode props), and build/app/app.bin
# does not change while we tune compression — so we can compress the existing app.bin
# many ways and compare sizes WITHOUT rebuilding. The winner's params then go into the
# Makefile, and the matching lc/lp/pb props byte (+ BCJ inverse) into the stub.
#
# Usage: scripts/debug/lzma_sweep.sh [path-to-app.bin]
#   defaults to build/app/app.bin
set -u

BIN="${1:-build/app/app.bin}"
if [ ! -f "$BIN" ]; then echo "no such file: $BIN (run make first)"; exit 1; fi
RAW=$(stat -c %s "$BIN")
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# props byte for a given lc/lp/pb: (pb*5 + lp)*9 + lc  (xz default lc3/lp0/pb2 = 0x5D)
props_byte() { printf "0x%02X" $(( ( $3 * 5 + $2 ) * 9 + $1 )); }

# label | extra-xz-flags (before --lzma1) | lzma1 opts (after dict)
# dict stays 128KiB (>= app size, so fully covered; matches the stub's dict props bytes)
run() {
  local label="$1" filt="$2" lc="$3" lp="$4" pb="$5" preset="$6"
  local opts="dict=128KiB,lc=${lc},lp=${lp},pb=${pb}"
  [ -n "$preset" ] && opts="preset=${preset},${opts}"
  # shellcheck disable=SC2086
  xz -f -c --format=raw $filt --lzma1=${opts} "$BIN" > "$TMP/out.lzma" 2>/dev/null
  local sz; sz=$(stat -c %s "$TMP/out.lzma")
  printf "%-34s %6d B   props=%s   (filt:%s)\n" "$label" "$sz" "$(props_byte "$lc" "$lp" "$pb")" "${filt:-none}"
}

echo "app.bin raw = $RAW B    (baseline below is the CURRENT Makefile setting)"
echo "------------------------------------------------------------------------"
run "baseline dict128 lc3/lp0/pb2"      ""           3 0 2 ""
run "preset9e lc3/lp0/pb2"              ""           3 0 2 "9e"
run "preset9e pb0"                      ""           3 0 0 "9e"
run "preset9e pb0 lc0"                  ""           0 0 0 "9e"
run "preset9e pb0 lc1"                  ""           1 0 0 "9e"
run "preset9e pb0 lc2"                  ""           2 0 0 "9e"
run "preset9e pb0 lc4"                  ""           4 0 0 "9e"
run "preset9e pb1"                      ""           3 0 1 "9e"
run "preset9e pb0 lp1 lc0"             ""           0 1 0 "9e"
echo "--- best non-BCJ candidates with DEFAULT preset ----------------------"
run "default lp1 lc0 pb0"               ""           0 1 0 ""
run "default lc3 lp0 pb1"               ""           3 0 1 ""
echo "--- ARMTHUMB BCJ, DEFAULT preset (preset9e proved worse) --------------"
run "armthumb default lc3/lp0/pb2"      "--armthumb" 3 0 2 ""
run "armthumb default pb0"              "--armthumb" 3 0 0 ""
run "armthumb default pb1"              "--armthumb" 3 0 1 ""
run "armthumb default lp1 lc0 pb0"      "--armthumb" 0 1 0 ""
run "armthumb default lp1 lc0 pb2"      "--armthumb" 0 1 2 ""
run "armthumb default lc2 lp0 pb2"      "--armthumb" 2 0 2 ""
echo "--- ARMTHUMB BCJ, neighbors of the winner (lp1 lc0 pb2) --------------"
run "armthumb lp1 lc0 pb2 (winner)"     "--armthumb" 0 1 2 ""
run "armthumb lp1 lc0 pb1"              "--armthumb" 0 1 1 ""
run "armthumb lp1 lc0 pb3"              "--armthumb" 0 1 3 ""
run "armthumb lp2 lc0 pb2"             "--armthumb" 0 2 2 ""
run "armthumb lp1 lc1 pb2 (best so far)" "--armthumb" 1 1 2 ""
run "armthumb lp0 lc0 pb2"             "--armthumb" 0 0 2 ""
run "armthumb lp1 lc1 pb1"             "--armthumb" 1 1 1 ""
run "armthumb lp1 lc1 pb0"             "--armthumb" 1 1 0 ""
run "armthumb lp1 lc2 pb2"             "--armthumb" 2 1 2 ""
run "armthumb lp2 lc1 pb2"             "--armthumb" 1 2 2 ""
