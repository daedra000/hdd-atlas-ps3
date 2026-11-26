# A folder standing for /dev_hdd0, for tools/host-test.sh: ~83,000 files (sparse, so big sizes
# take no disk), a 20,000-entry folder, a path past lv2's 1,024 bytes, a link back to the top,
# and folders named LOCKED (fsx_host.c refuses to open those, as the PS3 does locked ones).
#   python3 tools/host/make-fake-hdd.py <dir>     (makes <dir>/dev_hdd0)
import os, sys, struct
R = os.path.join(sys.argv[1], 'dev_hdd0')
def sfo(path, entries):
    # a PARAM.SFO: header, index (16 bytes each), keys, then values (strings utf-8, 4-aligned)
    keys = b''; vals = b''; idx = b''
    for k, v in entries:
        if isinstance(v, int): data = struct.pack('<I', v); fmt = 0x0404; dlen = 4; dmax = 4
        else:
            data = v.encode() + b'\0'; fmt = 0x0204; dlen = len(data); dmax = (dlen + 3) & ~3
            data += b'\0' * (dmax - dlen)
        idx += struct.pack('<HHIII', len(keys), fmt, dlen, dmax, len(vals))
        keys += k.encode() + b'\0'; vals += data
    while len(keys) % 4: keys += b'\0'
    kstart = 20 + len(idx)
    out = b'\0PSF' + struct.pack('<IIII', 0x101, kstart, kstart + len(keys), len(entries)) + idx + keys + vals
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(out)
def icon(path, n):
    from PIL import Image, ImageDraw
    im = Image.new('RGB', (320, 176), ((n * 53) % 256, (n * 97) % 256, (n * 151) % 256))
    ImageDraw.Draw(im).ellipse([110, 38, 210, 138], fill=(255, 255, 255))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)
def f(p, size):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as h: h.truncate(size)
for g in range(200):
    for d in range(10):
        for k in range(30): f(f"{R}/game/BLUS3{g:04d}/USRDIR/d{d}/f{k}.dat", 1000 + g * 7 + k)
f(f"{R}/game/BCES01585/USRDIR/CHUNKS/huge.psarc", 41 * 2**30)
f(f"{R}/GAMES/Uncharted 3/PS3_GAME/USRDIR/a.dat", 25 * 2**30)
f(f"{R}/PS3ISO/Uncharted.iso", 38 * 2**30)
with open(os.fsencode(f"{R}/theme/") + b"PlayStation\xaePlus Theme.p3t", "wb") as h: h.truncate(5000)		# (a name in Windows-1252, as some themes install)
for n in ("config.xml", "covers.xml", "songs_4_0.xml"): f(f"{R}/FileSystem/Export/{n}", 2000)		# (SingStar's songs, as a console had them)
f(f"{R}/FileSystem/Export/60001/video.mp4", 190000)
f(f"{R}/HDDAtlas/HDDAtlas_hdd0_report.txt", 100)		# (HDD Atlas's own: a report, and the recycle bin)
f(f"{R}/HDDAtlas/bin/1700000000_1/Binned Game.iso", 3 * 2**30)
with open(f"{R}/HDDAtlas/bin/bin.txt", "w") as h: h.write(f"1\t1700000000_1\t1700000000\t{3 * 2**30}\t1\t0\tPS3ISO/Binned Game.iso\t\t0\n")
for s in range(300): f(f"{R}/home/00000001/savedata/BLUS3{s:04d}-SAVE/DATA.BIN", 50000)
for t in range(50): f(f"{R}/home/00000001/trophy/NPWR{t:05d}_00/TROPHY.TRP", 300000)
f(f"{R}/home/00000001/exdata/UP0001-X.rap", 16); f(f"{R}/home/00000001/localusername", 8)
f(f"{R}/home/00000002/savedata/X/DATA", 10)
for t in range(20): f(f"{R}/theme/t{t}.p3t", 3 * 2**20)
for m in range(3000): f(f"{R}/music/{m // 100:03d}/{m}.mp3", 4 * 2**20)
f(f"{R}/packages/GTA_update.pkg", 4 * 2**30); f(f"{R}/tmp/wmtmp/cache.bin", 1000)
f(f"{R}/tmp/LOCKED/secret", 1000); os.makedirs(f"{R}/game/LOCKED", exist_ok=True)
f(f"{R}/vsh/task/x", 1); f(f"{R}/vsh/task/00000001/d0.pdb", 64)
# a download not installed yet (vsh/task/<n>: its .pkg, icon and d0.pdb, the title at entry 0x69) and a package waiting to be installed (vsh/game_pkg/<n>)
# (titles as pkgi and IRISMAN write them, decorated: "\u2605 Download "<name>""; no title: the package's
# file name, entry 0xCB)
def pdb(path, title, file=None):
    def rec(i, s): t = s.encode() + b"\0"; return struct.pack(">III", i, len(t), len(t)) + t
    body = (rec(0x69, title) if title else b"") + (rec(0xCB, file) if file else b"")
    with open(path, "wb") as h: h.write(b"\0\0\0\0" + struct.pack(">III", 0x64, 4, 4) + b"\0\0\0\0" + body)
# packages with a real header (magic, the content ID at 0x30): one of an installed game, one not
def pkg(path, cid):
    with open(path, "wb") as h: h.write(b"\x7fPKG" + b"\x80\x00\x00\x01" + bytes(0x28) + cid.encode().ljust(0x30, b"\0") + bytes(0x100))
pkg(f"{R}/packages/Fake1.pkg", "UP0001-BLUS30001_00-FAKEGAME00000001"); pkg(f"{R}/packages/NotYet.pkg", "UP0001-NPUB99999_00-NOTINSTALLED0001")
f(f"{R}/vsh/task/00000002/F1.pkg", 3 * 2**20); pdb(f"{R}/vsh/task/00000002/d0.pdb", "\u2605 Download \"Fake Download\"")
f(f"{R}/vsh/task/10000003/UP0001-NPUB31111_00-NOTITLE000000001.pkg", 2 * 2**20); pdb(f"{R}/vsh/task/10000003/d0.pdb", None, "UP0001-NPUB31111_00-NOTITLE000000001.pkg")
f(f"{R}/vsh/task/notatask/x.bin", 2 * 2**20)
f(f"{R}/vsh/game_pkg/80000000/P1.pkg", 2 * 2**20); pdb(f"{R}/vsh/game_pkg/80000000/d0.pdb", "\u2605 Install \"Fake Queued Game\"")
f(f"{R}/mms/db", 2 * 2**20); f(f"{R}/.toplevelfile", 123)
p = f"{R}/data/deep"
for i in range(60): p += f"/{'d' * 20}{i:02d}"
os.makedirs(p, exist_ok=True); open(p + "/end", "w").write("x")
for i in range(20000): f(f"{R}/tmp/many/{i}", 1)
if not os.path.islink(f"{R}/data/loop"): os.symlink(R, f"{R}/data/loop")
# titles and icons (PARAM.SFO, ICON0.PNG, TROPCONF.SFM), and what the console's HDD showed
for g in range(12):
    sfo(f"{R}/game/BLUS3{g:04d}/PARAM.SFO", [("CATEGORY", "HG" if g % 4 else "GD"), ("TITLE", f"Fake Game {g}\nSubtitle on a second line"), ("TITLE_ID", f"BLUS3{g:04d}"), ("VERSION", "01.00")])
    if g % 3 != 2: icon(f"{R}/game/BLUS3{g:04d}/ICON0.PNG", g)
sfo(f"{R}/game/BCES01585/PARAM.SFO", [("CATEGORY", "HG"), ("TITLE", "The Last of Us™"), ("TITLE_ID", "BCES01585")])
icon(f"{R}/game/BCES01585/ICON0.PNG", 99)
sfo(f"{R}/GAMES/Uncharted 3/PS3_GAME/PARAM.SFO", [("CATEGORY", "DG"), ("TITLE", "Uncharted 3: Drake's Deception"), ("TITLE_ID", "BCES01175")])
icon(f"{R}/GAMES/Uncharted 3/PS3_GAME/ICON0.PNG", 7)
for s_ in range(8):
    sfo(f"{R}/home/00000001/savedata/BLUS3{s_:04d}-SAVE/PARAM.SFO", [("CATEGORY", "SD"), ("TITLE", f"Fake Game {s_}"), ("SUB_TITLE", f"Slot {s_ + 1}"), ("TITLE_ID", f"BLUS3{s_:04d}")])
    icon(f"{R}/home/00000001/savedata/BLUS3{s_:04d}-SAVE/ICON0.PNG", 40 + s_)
for t in range(5):
    open(f"{R}/home/00000001/trophy/NPWR{t:05d}_00/TROPCONF.SFM", "w").write(f"<?xml version='1.0'?><trophyconf><title-name>Trophies &amp; Things {t}</title-name></trophyconf>")
open(f"{R}/home/00000001/localusername", "w").write("Euan")
f(f"{R}/ROMS/snes/game.smc", 2 * 2**20)
f(f"{R}/screenshot_2026_09_25_12_19_14.bmp", 3686454)
f(f"{R}/boot_plugins.txt", 40)
# disc images with something inside to read (iso.c): a PS3 ISO (PS3_GAME/PARAM.SFO), a PS2 one
# (SYSTEM.CNF), a raw PS1 .bin (2352-byte sectors), one with its ID only in its name; and a
# duplicate (the same title installed and backed up), an orphaned update and save
def iso9660(path, files, raw=False):
    # files: {"PS3_GAME/PARAM.SFO": bytes, "SYSTEM.CNF": bytes}; a minimal image: PVD at 16, root at 18
    SEC = 2048
    def rec(name, lba, size, isdir):
        n = name.encode()
        l = 33 + len(n) + (1 - len(n) % 2)
        r = bytearray(l); r[0] = l
        r[2:6] = lba.to_bytes(4, 'little'); r[6:10] = lba.to_bytes(4, 'big')
        r[10:14] = size.to_bytes(4, 'little'); r[14:18] = size.to_bytes(4, 'big')
        r[25] = 2 if isdir else 0; r[32] = len(n); r[33:33 + len(n)] = n
        return bytes(r)
    dirs = sorted(set(k.split('/')[0] for k in files if '/' in k))
    sectors = {}
    lba = 19 + len(dirs)
    data_lba = {}
    for k in files:
        data_lba[k] = lba; lba += (len(files[k]) + SEC - 1) // SEC
    root = rec('\0', 18, SEC, True) + rec('\1', 18, SEC, True)
    for i, d in enumerate(dirs): root += rec(d, 19 + i, SEC, True)
    for k in files:
        if '/' not in k: root += rec(k + ';1', data_lba[k], len(files[k]), False)
    sectors[18] = root
    for i, d in enumerate(dirs):
        sub = rec('\0', 19 + i, SEC, True) + rec('\1', 18, SEC, True)
        for k in files:
            if k.startswith(d + '/'): sub += rec(k.split('/')[1] + ';1', data_lba[k], len(files[k]), False)
        sectors[19 + i] = sub
    pvd = bytearray(SEC); pvd[0] = 1; pvd[1:6] = b'CD001'; pvd[6] = 1; pvd[156:156 + 34] = rec('\0', 18, SEC, True)
    sectors[16] = bytes(pvd)
    for k in files:
        for j in range(0, len(files[k]), SEC): sectors[data_lba[k] + j // SEC] = files[k][j:j + SEC]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as h:
        for n in range(lba + 4):
            body = sectors.get(n, b'').ljust(SEC, b'\0')
            h.write(b'\0' * 24 + body + b'\0' * 280 if raw else body)
def sfo_bytes(entries):
    import tempfile
    t = tempfile.NamedTemporaryFile(delete=False); t.close(); sfo(t.name, entries)
    b = open(t.name, 'rb').read(); os.unlink(t.name); return b
import io
from PIL import Image as _I
_png = io.BytesIO(); _I.new('RGB', (320, 176), (200, 60, 40)).save(_png, 'PNG')
iso9660(f"{R}/PS3ISO/Game One.iso", {"PS3_GAME/PARAM.SFO": sfo_bytes([("CATEGORY", "DG"), ("TITLE", "Fake Game 3"), ("TITLE_ID", "BLUS30003")]), "PS3_GAME/ICON0.PNG": _png.getvalue()})
# the same game's ISO in webMAN's "stealth" video/ folder; an app's cache, a disc's system update, an update waiting
iso9660(f"{R}/video/Stealth.iso", {"PS3_GAME/PARAM.SFO": sfo_bytes([("CATEGORY", "DG"), ("TITLE", "Fake Game 3"), ("TITLE_ID", "BLUS30003")])})
sfo(f"{R}/game/IRISMAN00/PARAM.SFO", [("CATEGORY", "HG"), ("TITLE", "IRISMAN"), ("TITLE_ID", "IRISMAN00")])
f(f"{R}/game/IRISMAN00/USRDIR/EBOOT.BIN", 500000); f(f"{R}/game/IRISMAN00/cache/BLES00001/big.666", 2 * 2**20)
f(f"{R}/GAMES/Uncharted 3/PS3_UPDATE/PS3UPDAT.PUP", 2 * 2**20)
f(f"{R}/updater/01/PS3UPDAT.PUP", 2 * 2**20); f(f"{R}/updater/01/notes.txt", 10); f(f"{R}/updater/02/sub/x.bin", 20)
iso9660(f"{R}/PS2ISO/Old PS2.iso", {"SYSTEM.CNF": b"BOOT2 = cdrom0:\\SLUS_203.12;1\r\nVER = 1.00\r\n"})
iso9660(f"{R}/PSXISO/Classic.bin", {"SYSTEM.CNF": b"BOOT = cdrom:\\SCUS_944.26;1\r\n"}, raw=True)
f(f"{R}/PS3ISO/Other Game [BCES00001].iso", 70000)
# an update for a game that isn't installed or backed up, and a save for one that isn't either
sfo(f"{R}/game/NPEB09999/PARAM.SFO", [("CATEGORY", "GD"), ("TITLE", "Gone Game Update"), ("TITLE_ID", "NPEB09999")])
f(f"{R}/game/NPEB09999/USRDIR/patch.dat", 3 * 2**20)
sfo(f"{R}/home/00000001/savedata/BLES77777-SAVE/PARAM.SFO", [("CATEGORY", "SD"), ("TITLE", "Lost Save"), ("SUB_TITLE", "Slot 1"), ("TITLE_ID", "BLES77777")])
sfo(f"{R}/home/00000001/minis_savedata/NPUZ00001DATA/PARAM.SFO", [("CATEGORY", "MS"), ("TITLE", "Fake Minis"), ("SUB_TITLE", "Progress"), ("TITLE_ID", "NPUZ00001")])
f(f"{R}/home/00000001/minis_savedata/NPUZ00001DATA/DATA.BIN", 4000)
sfo(f"{R}/home/00000001/ps2emu2_savedata/NPEB00001/PARAM.SFO", [("CATEGORY", "2P"), ("TITLE", "Fake PS2 Classic"), ("TITLE_ID", "SLES50001")])
f(f"{R}/home/00000001/ps2emu2_savedata/NPEB00001/SCEVMC0.VME", 8000)		# (a PS2 Classic's save: one item, as savedata's are)		# (a PSP Minis save: one item, as savedata's are)
f(f"{R}/home/00000001/savedata/BLES77777-SAVE/DATA.BIN", 2 * 2**20)
# old times: a game untouched since 2023 and one whose save is from 2024 (for "not played lately")
import glob as _g
for path in _g.glob(f"{R}/game/BLUS30011/**/*", recursive=True): os.utime(path, (1672531200, 1672531200))		# 2023-01-01
for path in _g.glob(f"{R}/game/BLUS30005/**/*", recursive=True): os.utime(path, (1700000000, 1700000000))
for path in _g.glob(f"{R}/home/00000001/savedata/BLUS30005-SAVE/*"): os.utime(path, (1717200000, 1717200000))	# 2024-06-01
# a USB stick too (its own folder, dev_usb000): a backup of a game that's also installed on the HDD
U = os.path.join(sys.argv[1], 'dev_usb000')
iso9660(f"{U}/PS3ISO/TLOU.iso", {"PS3_GAME/PARAM.SFO": sfo_bytes([("CATEGORY", "DG"), ("TITLE", "The Last of Us"), ("TITLE_ID", "BCES01585")])})
f(f"{U}/music/song.mp3", 5 * 2**20)
print("made", R, "and", U)
