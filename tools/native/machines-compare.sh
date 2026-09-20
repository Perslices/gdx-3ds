#!/usr/bin/env bash
# machines-compare.sh <runA> <runB>: byte-compare the mx_* SHOT BMPs of two
# /tmp/machines-art/<run> directories and summarise the [native] receipts of each log.
A="/tmp/machines-art/$1"; B="/tmp/machines-art/$2"
echo "== SHOT byte-compare $1 vs $2 (scanout twins; the frame-mirror .bmp is black in-game)"
for f in "$A"/mx_*_scan.bmp; do
    n=$(basename "$f")
    if [ ! -f "$B/$n" ]; then echo "  $n: missing in $2"; continue; fi
    if cmp -s "$f" "$B/$n"; then echo "  $n: IDENTICAL ($(stat -f %z "$f") B)";
    else echo "  $n: DIFFER ($(cmp -l "$f" "$B/$n" | wc -l | tr -d ' ') bytes of $(stat -f %z "$f"))"; fi
done
for r in "$1" "$2"; do
    echo "== $r receipts"; grep -h "\[native\]" "/tmp/machines-art/$r/log.txt" | grep -v "bake slot" | head -12
    echo "   bakes: $(grep -c 'bake slot' "/tmp/machines-art/$r/log.txt") fails: $(grep -c 'bake FAIL' "/tmp/machines-art/$r/log.txt")"
    grep -h "\[fatal\]\|\[gfxfail\]\|vtx-failsafe" "/tmp/machines-art/$r/log.txt" | head -3
done
