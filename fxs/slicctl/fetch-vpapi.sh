#!/bin/sh
# Fetch Microsemi VP-API-II 2.24.0.lite (vp886 family) and the userspace HAL
# headers from the bcm63xx-phone project, then configure them for this board.
#
# These files are (c) Microsemi and carry no redistribution licence, so they
# are not committed here; this script pulls them from the public GPL project
# that bundles them. Re-run any time; it overwrites ./vpapi and ./arch/*.h.
set -e
cd "$(dirname "$0")"
REPO=https://github.com/pgid69/bcm63xx-phone.git
COMMIT=a49d39ee17c6b083a48ed583c3644f154127b593
SRC=vendor/bcm63xx-phone
Z=$SRC/bcm63xx-phone/src/extern/zarlink/2.24.0.lite

if [ ! -d "$SRC/.git" ]; then
	mkdir -p vendor
	git clone -q "$REPO" "$SRC"
fi
git -C "$SRC" checkout -q "$COMMIT"

rm -rf vpapi
mkdir -p vpapi
cp -r "$Z/api_lib/common" "$Z/api_lib/includes" "$Z/api_lib/vp886_api" vpapi/
cp "$Z"/arch/ztap/userspace/vp_api_types.h "$Z"/arch/ztap/userspace/vp_api_profile_type.h \
   "$Z"/arch/ztap/userspace/sys_service.h "$Z"/arch/ztap/userspace/vp_hal.h \
   "$Z"/arch/ztap/userspace/mpi_hal.h "$Z"/arch/ztap/userspace/hbi_hal.h arch/

CFG=vpapi/includes/vp_api_cfg.h
# 1) build the ZL880/miSLIC (886/887) family instead of the bcm63xx's 880
sed -i 's|^  #define VP_CC_880_SERIES   /\*\*<|  #undef VP_CC_880_SERIES   /**<|' "$CFG"
sed -i 's|^  #undef VP_CC_886_SERIES   /\*\*<|  #define VP_CC_886_SERIES   /**<|' "$CFG"
# 2) simple polled mode: no SLIC interrupt line is wired to a known GPIO
sed -i 's/^#undef  VP886_SIMPLE_POLLED_MODE /#define VP886_SIMPLE_POLLED_MODE /' "$CFG"
sed -i 's/^#define VP886_EFFICIENT_POLLED_MODE /#undef  VP886_EFFICIENT_POLLED_MODE /' "$CFG"

grep -q '^  #define VP_CC_886_SERIES' "$CFG" && grep -q '^#define VP886_SIMPLE_POLLED_MODE' "$CFG" \
	|| { echo "config edit failed: check $CFG"; exit 1; }
echo "VP-API-II ready in ./vpapi (886 family, simple polled)"
