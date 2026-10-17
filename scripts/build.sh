#!/bin/sh
# Build VAWT for Digitone mk1 OS 1.43 and 1.44: core + machines + vawt,
# lint each combination, patch test images into out/.
# Usage: sh scripts/build.sh STOCK43 [STOCK44 [MODVER]]
# MODVER stamps the build (ELE3 needs exactly 4 characters) and names it:
# out/vawt-1.43-0.1a.syx. It must match vawt's ele3_version.
set -e
STOCK43="${1:?usage: build.sh STOCK43 [STOCK44 [MODVER]]}"
STOCK44="${2:-}"
MODVER="${3:-0.1z}"
EL=tools/elekloader
export ELEKLOADER_CROSS="${ELEKLOADER_CROSS:-m68k-elf-}"
export PYTHONPATH="$EL:${PYTHONPATH:-}"
mkdir -p out
echo "== 1.43 =="
python3 -m elekloader.sdk.build "$EL/mods/core-dn1" --stock "$STOCK43"
python3 -m elekloader.sdk.build "$EL/mods/machines-dn1" --stock "$STOCK43"
python3 -m elekloader.sdk.build vawt --stock "$STOCK43"
CORE43=$(ls -t "$EL/mods/core-dn1/out"/core-*.elemod | grep -v -- '-os' | head -n 1)
MACH43=$(ls -t "$EL/mods/machines-dn1/out"/machines-*.elemod | grep -v -- '-os' | head -n 1)
VAWT43=$(ls -t vawt/out/dnvawt-*.elemod | grep -v -- '-os' | head -n 1)
python3 -m elekloader.lint "$VAWT43" \
  --stock "$STOCK43" \
  --with "$CORE43" \
  --with "$MACH43"
python3 -m elekloader.patch --stock "$STOCK43" \
  --mod "$CORE43" \
  --mod "$MACH43" \
  --mod "$VAWT43" \
  --out "out/vawt-1.43-$MODVER.syx" --version "$MODVER"
if [ -n "$STOCK44" ]; then
echo "== 1.44 =="
python3 -m elekloader.sdk.build "$EL/mods/core-dn1" --stock "$STOCK44"
python3 -m elekloader.sdk.build "$EL/mods/machines-dn1" --stock "$STOCK44"
python3 -m elekloader.sdk.build vawt --stock "$STOCK44"
CORE44=$(ls -t "$EL/mods/core-dn1/out"/core-*-os1.44.elemod | head -n 1)
MACH44=$(ls -t "$EL/mods/machines-dn1/out"/machines-*-os1.44.elemod | head -n 1)
VAWT44=$(ls -t vawt/out/dnvawt-*-os1.44.elemod | head -n 1)
python3 -m elekloader.lint "$VAWT44" \
  --stock "$STOCK44" \
  --with "$CORE44" \
  --with "$MACH44"
python3 -m elekloader.patch --stock "$STOCK44" \
  --mod "$CORE44" \
  --mod "$MACH44" \
  --mod "$VAWT44" \
  --out "out/vawt-1.44-$MODVER.syx" --version "$MODVER"
fi
echo BUILT
