#!/bin/sh
# Copyright © 2026 CCP ehf.
#
#   check_deterministic.sh <blue-metadump> <work dir>
#
# Runs blue-metadump twice, each in its own process (blue starts once per process), and fails
# unless both metadata.json files are byte-identical.
set -eu

dump="$1"
work="$2"

rm -rf "$work"
mkdir -p "$work"
"$dump" --out "$work/first.json" --blue-commit test --blueexposure-commit test
"$dump" --out "$work/second.json" --blue-commit test --blueexposure-commit test
if ! cmp "$work/first.json" "$work/second.json"; then
    diff "$work/first.json" "$work/second.json" | head -n 40
    exit 1
fi
echo "two runs gave identical output ($(wc -c < "$work/first.json") bytes)"
