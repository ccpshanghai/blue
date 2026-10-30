#!/bin/sh
# Copyright © 2026 CCP ehf.
#
#   check_metadata.sh <blue-metadump> <work dir> <class count>
#
# Runs blue-metadump and checks its metadata.json (schema 1: two-space indentation, one key per
# line, so classes' keys sit at 6 spaces and their members' and methods' keys at 10):
#   - it lists <class count> classes, the expected blue_class_count();
#   - MemStream has its attributes size and pos (uint64, read-only) and the IBlueStream methods
#     Read, Write and Seek;
#   - no method or function is "callable": false, i.e. every type has a C conversion.
set -eu

dump="$1"
work="$2"
expected="$3"

rm -rf "$work"
mkdir -p "$work"
json="$work/metadata.json"
"$dump" --out "$json" --blue-commit test --blueexposure-commit test

status=0
fail() {
    echo "FAIL: $*"
    status=1
}

head -n 1 "$json" | grep -qx '{' || fail "not a JSON object"
grep -qx '  "schemaVersion": 1,' "$json" || fail "schemaVersion is not 1"

classes="$(awk '
    /^  "classes": \[/ { inside = 1; next }
    inside && /^  \]/ { inside = 0 }
    inside && /^      "name": / { n++ }
    END { print n + 0 }' "$json")"
[ "$classes" -eq "$expected" ] || fail "${classes} classes, expected ${expected}"

# "<section> <name>" for every member and method of MemStream, plus "<key> <member> <value>"
# for its members' type and access
memstream="$(awk '
    function value(line) { sub(/^[^:]*: "/, "", line); sub(/",?$/, "", line); return line }
    /^  "classes": \[/ { inside = 1; next }
    inside && /^  \]/ { inside = 0 }
    !inside { next }
    /^      "name": / { current = (value($0) == "MemStream"); section = ""; next }
    !current { next }
    /^      "[a-z]+": \[/ { section = $1; gsub(/[":]/, "", section); next }
    /^          "name": / { name = value($0); print section " " name; next }
    section == "members" && /^          "(type|access)": / { key = $1; gsub(/[":]/, "", key); print key " " name " " value($0) }
' "$json")"
[ -n "$memstream" ] || fail "MemStream is missing"
for want in "members size" "members pos" "type size uint64" "type pos uint64" "access size read" "access pos read" \
    "methods Read" "methods Write" "methods Seek"; do
    printf '%s\n' "$memstream" | grep -qx "$want" || fail "MemStream: no \"${want}\""
done

callable="$(grep -c '"callable": true' "$json" || true)"
uncallable="$(grep -c '"callable": false' "$json" || true)"
[ "$callable" -gt 0 ] || fail "no callable method"
if [ "$uncallable" -ne 0 ]; then
    fail "${uncallable} entries are \"callable\": false"
    grep -n -B 8 '"callable": false' "$json" | grep '"name"\|"returns"\|unknown:' | head -n 40
fi

if [ "$status" -eq 0 ]; then
    echo "metadata.json: ${classes} classes, MemStream complete, ${callable} callable methods and functions, none uncallable"
fi
exit "$status"
