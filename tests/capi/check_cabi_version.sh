#!/bin/sh
# Copyright © 2026 CCP ehf.
#
#   check_cabi_version.sh <cc> <c++> <include dir holding blue_c/>
#
# blue.h is written against the 1.x value types of BlueCAbi.h and must not compile with another
# major version: a copy of the installed pair whose BlueCAbi.h says major 2 must fail with
# blue.h's #error. The unchanged pair must compile, as C99, C11 and C++11 (so the failure is the
# version check, and the layout asserts compile in every mode the header supports).
set -eu

cc="$1"
cxx="$2"
include="$3"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/v1/blue_c" "$work/v2/blue_c"
cp "$include/blue_c/blue.h" "$include/blue_c/BlueCAbi.h" "$work/v1/blue_c/"
cp "$include/blue_c/blue.h" "$work/v2/blue_c/"
sed 's/^#define BLUE_CABI_VERSION_MAJOR 1$/#define BLUE_CABI_VERSION_MAJOR 2/' "$include/blue_c/BlueCAbi.h" > "$work/v2/blue_c/BlueCAbi.h"
if cmp -s "$include/blue_c/BlueCAbi.h" "$work/v2/blue_c/BlueCAbi.h"; then
    echo "BlueCAbi.h has no '#define BLUE_CABI_VERSION_MAJOR 1' line to change"
    exit 1
fi

printf '#include "blue_c/blue.h"\nint main( void ) { return (int)( blue_abi_version() >> 16 ); }\n' > "$work/probe.c"
cp "$work/probe.c" "$work/probe.cpp"

status=0
for mode in c99 c11; do
    if ! "$cc" -std="$mode" -Wall -Wextra -pedantic-errors -fsyntax-only -I"$work/v1" "$work/probe.c" > "$work/out" 2>&1; then
        echo "blue.h with BlueCAbi.h 1.x does not compile as ${mode}:"
        cat "$work/out"
        status=1
    fi
done
if ! "$cxx" -std=c++11 -Wall -Wextra -pedantic-errors -fsyntax-only -I"$work/v1" "$work/probe.cpp" > "$work/out" 2>&1; then
    echo "blue.h with BlueCAbi.h 1.x does not compile as C++11:"
    cat "$work/out"
    status=1
fi

if "$cc" -std=c11 -fsyntax-only -I"$work/v2" "$work/probe.c" > "$work/out" 2>&1; then
    echo "blue.h compiles with BLUE_CABI_VERSION_MAJOR 2"
    status=1
elif ! grep -q 'needs BlueCAbi.h 1.x' "$work/out"; then
    echo "blue.h fails with BLUE_CABI_VERSION_MAJOR 2, but not with its version #error:"
    cat "$work/out"
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "blue.h compiles with BlueCAbi.h 1.x (C99, C11, C++11) and refuses major 2"
fi
exit "$status"
