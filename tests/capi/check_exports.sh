#!/bin/sh
# Copyright © 2026 CCP ehf.
#
#   check_exports.sh <nm> <blue.so> <blue_c/blue.h>
#
# With BLUE_CAPI_EXPORTS_ONLY, blue.so's dynamic symbol table holds exactly the functions
# blue.h declares. Nothing else is allowed: the carbon runtime libraries blue.so loads
# (libCcpCore.so, libTracyClient.so) import no symbol from it.
set -eu

nm="$1"
so="$2"
header="$3"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Defined, global dynamic symbols; "A" is a version node, not a symbol
"$nm" -D --defined-only --extern-only "$so" | awk '$2 != "A" { print $3 }' | LC_ALL=C sort -u > "$work/exported"
sed -n 's/.*BLUE_CALL \(blue_[A-Za-z0-9_]*\)[[:space:]]*(.*/\1/p' "$header" | LC_ALL=C sort -u > "$work/declared"

status=0
count="$(wc -l < "$work/declared")"
if [ "$count" -eq 0 ]; then
    echo "no blue_* declarations found in ${header}"
    status=1
fi

grep -v '^blue_' "$work/exported" > "$work/others" || true
if [ -s "$work/others" ]; then
    echo "exported but not part of the C API ($(wc -l < "$work/others"), first 50):"
    head -n 50 "$work/others"
    status=1
fi

LC_ALL=C comm -23 "$work/declared" "$work/exported" > "$work/missing"
if [ -s "$work/missing" ]; then
    echo "declared in blue.h but not exported:"
    cat "$work/missing"
    status=1
fi

grep '^blue_' "$work/exported" | LC_ALL=C comm -13 "$work/declared" - > "$work/extra" || true
if [ -s "$work/extra" ]; then
    echo "exported but not declared in blue.h:"
    cat "$work/extra"
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "blue.so exports exactly the ${count} functions blue.h declares"
fi
exit "$status"
