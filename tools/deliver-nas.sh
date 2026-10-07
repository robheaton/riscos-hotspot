#!/bin/bash
#
# Copies the build to the NAS folder the RISC OS machine reads from, then
# downloads everything again and compares every file byte for byte.
#
#   tools/deliver-nas.sh          (after make dist)
#
# Delivered to smb://nas1.local/RISCOS/Development/Hotspot (nas1.local is
# 10.0.0.2; anonymous access, no password):
#
#   !Hotspot/            the application, files with their ,xxx type suffixes
#   Hotspot-<ver>.zip    the same, zipped with RISC OS file types
#   ReadMe,fff           what to do first

set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/riscos"
app="$build/!Hotspot"
share="//nas1.local/RISCOS"
dest="Development/Hotspot"

version="$(sed -n 's/^#define APP_VERSION *"\(.*\)"/\1/p' "$root/src/ro.h")"
zip="$build/Hotspot-$version.zip"
readme="$root/app/ReadMe,fff"

[ -d "$app" ] || { echo "nothing built yet: run make dist first" >&2; exit 1; }
[ -f "$zip" ] || { echo "no $zip: run make dist first" >&2; exit 1; }
[ -f "$readme" ] || { echo "no $readme" >&2; exit 1; }

# The folders may already be there; that is fine.
smbclient "$share" -N -c "mkdir \"$dest\"" >/dev/null 2>&1 || true
smbclient "$share" -N -c "cd \"$dest\"; mkdir \"!Hotspot\"" >/dev/null 2>&1 || true

# The application folder, the zip, the ReadMe.
smbclient "$share" -N \
    -c "cd \"$dest/!Hotspot\"; lcd \"$app\"; prompt OFF; mput *" >/dev/null
smbclient "$share" -N \
    -c "cd \"$dest\"; lcd \"$build\"; put \"Hotspot-$version.zip\"" >/dev/null
smbclient "$share" -N \
    -c "cd \"$dest\"; lcd \"$root/app\"; put \"ReadMe,fff\"" >/dev/null

# Fetch it all back and compare.
check="$(mktemp -d)"
trap 'rm -rf "$check"' EXIT
mkdir "$check/app"

smbclient "$share" -N \
    -c "cd \"$dest/!Hotspot\"; lcd \"$check/app\"; prompt OFF; mget *" >/dev/null
smbclient "$share" -N \
    -c "cd \"$dest\"; lcd \"$check\"; get \"Hotspot-$version.zip\"; get \"ReadMe,fff\"" \
    >/dev/null

bad=0

compare() {     # compare <local file> <downloaded file>
    local name
    name="$(basename "$1")"
    if [ -f "$2" ] && cmp -s "$1" "$2"; then
        printf '  OK   %-24s %8d bytes  md5 %s\n' "$name" "$(stat -c %s "$1")" \
            "$(md5sum "$1" | cut -d' ' -f1)"
    else
        printf '  FAIL %-24s differs or missing on the NAS\n' "$name"
        bad=1
    fi
}

for f in "$app"/*; do
    compare "$f" "$check/app/$(basename "$f")"
done
compare "$zip" "$check/Hotspot-$version.zip"
compare "$readme" "$check/ReadMe,fff"

# Nothing extra left over from an earlier delivery in the application folder.
for f in "$check"/app/*; do
    [ -e "$app/$(basename "$f")" ] || {
        printf '  NOTE %-24s is on the NAS but is not part of this build\n' \
            "$(basename "$f")"
    }
done

echo
smbclient "$share" -N -c "cd \"$dest\"; ls" 2>/dev/null | grep -v blocks
smbclient "$share" -N -c "cd \"$dest/!Hotspot\"; ls" 2>/dev/null | grep -v blocks

if [ "$bad" -ne 0 ]; then
    echo "DELIVERY CHECK FAILED" >&2
    exit 1
fi

echo "delivered to smb://nas1.local/RISCOS/$dest and verified"
