#!/bin/sh
# HDD Atlas's PKG, in WSL, with the toolchain PS3 World's tools/wsl-toolchain.sh put in /opt/ps3
# (and pkg.py + the patched librsx its wsl-build.sh made once). Builds in place: everything
# it makes is under build/ (and the PKG is copied to out/).
#   sh tools/build.sh
set -e
TOP=/opt/ps3
export PS3DEV=$TOP/ps3dev PSL1GHT=$TOP/ps3dev
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH"
PY=$TOP/psl1ght-py
[ -x "$PS3DEV/ppu/bin/ppu-gcc" ] || { echo "no toolchain in $PS3DEV"; exit 1; }
[ -e "$TOP/.done-ps3py" ] || { echo "pkg.py isn't set up: run PS3 World's tools/wsl-build.sh once"; exit 1; }
cd "$(dirname "$0")/.."
mkdir -p build out
ppu-gcc --version | head -1
make -f Makefile.ps3 -j"$(nproc)" self > build/build.log 2>&1 || {
	grep -E 'error|undefined reference|multiple definition|No rule|Error [0-9]' build/build.log | awk '!seen[$0]++' | head -60
	exit 1
}
grep -E 'warning' build/build.log | awk '!seen[$0]++' | head -20 || true
echo "r2off stubs (must be 0): $(ppu-nm build/ps3/hddatlas.elf | grep -c r2off || true)"
make -f Makefile.ps3 PS3PY_DIR="$PY/tools/ps3py" pkg > build/pkg.log 2>&1 || { tail -20 build/pkg.log; exit 1; }
for f in USRDIR/EBOOT.BIN USRDIR/data/fonts/Poppins-Regular.ttf USRDIR/data/logo.png USRDIR/data/backdrop.png \
	USRDIR/data/glyphs/cross.png USRDIR/data/music.ogg USRDIR/data/lang/es.txt USRDIR/data/lang/ja.txt USRDIR/data/flags/gb.png USRDIR/data/menu/free.png USRDIR/data/menu/scan.png USRDIR/data/licenses/GPL-2.0.txt USRDIR/data/licenses/MUSIC.txt PIC1.PNG ICON0.PNG PARAM.SFO; do
	[ -e "build/ps3/package/pkg/$f" ] || { echo "missing from the PKG: $f"; exit 1; }
done
V=$(sed -n 's/.*APP_VERSION[^"]*"\(.*\)".*/\1/p' src/version.h)
cp build/ps3/hddatlas.gnpdrm.pkg "out/HDDAtlas-v$V.pkg"
ls -la "out/HDDAtlas-v$V.pkg"
