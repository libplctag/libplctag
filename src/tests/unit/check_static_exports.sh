#!/bin/sh
#
# Checks that the static library exports no plc_tag_* function (issue #692): a shared library
# that links it must not re-export the libplctag API.  Usage: check_static_exports.sh <libplctag_static.a>
#
# ponytail: Mach-O (nm -m) and ELF (readelf) only, the POSIX platforms we build on.

lib="$1"

if [ ! -f "$lib" ]; then
    echo "FAIL: no library at \"$lib\"."
    exit 1
fi

if [ "$(uname)" = "Darwin" ]; then
    syms=$(nm -m "$lib") || exit 1
    exported=$(echo "$syms" | grep -v "(undefined)" | grep ") external _plc_tag_")
    hidden=$(echo "$syms" | grep -c "private external _plc_tag_create$")
else
    syms=$(readelf -sW "$lib") || exit 1
    exported=$(echo "$syms" | awk '$5 == "GLOBAL" && $6 == "DEFAULT" && $7 != "UND" && $8 ~ /^plc_tag_/')
    hidden=$(echo "$syms" | awk '$5 == "GLOBAL" && $6 == "HIDDEN" && $7 != "UND" && $8 == "plc_tag_create"' | wc -l)
fi

if [ -n "$exported" ]; then
    echo "FAIL: the static library exports these plc_tag_* functions:"
    echo "$exported"
    exit 1
fi

# guards against the patterns above silently matching nothing.
if [ "$hidden" -lt 1 ]; then
    echo "FAIL: plc_tag_create was not found as a hidden symbol."
    exit 1
fi

echo "PASS: no plc_tag_* function is exported from the static library."
exit 0
