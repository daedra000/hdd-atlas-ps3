#!/bin/sh
# The scanner and the screens on the PC (no PS3): builds tools/host's programs into build/host,
# makes a fake /dev_hdd0 under build/host (once), scans it with AddressSanitizer + UBSan on a
# 64 KB thread stack (the PS3's), stops a scan part way, tries a 100-item cap, and draws the
# overview at 1280 and 960 (the 4:3 layout) as build/host/*.png. probe_test runs fsx_ps3.c's
# cellFsGetDirectoryEntries probe against mock firmware calls (tools/host/mock).
#   sh tools/host-test.sh
set -e
cd "$(dirname "$0")/.."
H=build/host
mkdir -p $H
[ -f $H/fake/.v16 ] || { python3 tools/host/make-fake-hdd.py $H/fake && touch $H/fake/.v16; }
# the fake HDD's recycle bin as made: its one entry, put in now (so nothing in it is old enough to go)
B=$H/fake/dev_hdd0/HDDAtlas/bin
find $B -mindepth 1 -maxdepth 1 -type d ! -name 1700000000_1 -exec rm -rf {} +
printf '1\t1700000000_1\t%s\t3221225472\t1\t0\tPS3ISO/Binned Game.iso\t\t0\n' "$(date +%s)" > $B/bin.txt
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -Werror=implicit-function-declaration"
SRC="src/scan.c src/sfo.c src/iso.c tools/host/fsx_host.c tools/host/plat_host.c"
REP="src/report.c src/fmt.c src/lang.c"
gcc -O1 -g -Wall -Wno-format-truncation $SAN -Isrc -o $H/scan_test $SRC tools/host/scan_test.c $REP -lpthread
gcc -O1 -g -Wno-format-truncation $SAN -DITEMS_MAX=100 -Isrc -o $H/scan_test_cap $SRC tools/host/scan_test.c $REP -lpthread
gcc -O1 -g -Wno-format-truncation $SAN -DDIRS_MAX=500 -Isrc -o $H/scan_test_dirs $SRC tools/host/scan_test.c $REP -lpthread
gcc -O1 -g -Wno-format-truncation $SAN -DSCAN_QUIET_US=500000ULL -DSCAN_GIVE_UP_US=2000000ULL -Isrc -o $H/scan_test_stuck $SRC tools/host/scan_test.c $REP -lpthread
gcc -O1 -g -Wno-format-truncation $SAN -DDIR_ENTRIES_MAX=5000 -Isrc -o $H/scan_test_entries $SRC tools/host/scan_test.c $REP -lpthread
gcc -O1 -g -Wno-format-truncation $SAN -DDIR_ENTRIES_MAX=20000 -Isrc -o $H/scan_test_exact $SRC tools/host/scan_test.c $REP -lpthread
SHOT_SRC="tools/host/shot.c src/ui_delete.c src/del.c tools/host/del_host.c src/settings.c src/ui_menu.c src/report.c src/i18n.c src/games.c src/history.c src/ui_map.c src/ui_home.c src/bin.c src/finds.c src/ui.c src/ui_list.c src/ui_health.c src/health.c tools/host/ata_host.c src/music.c src/icons.c src/jobs.c src/ui_kit.c src/fmt.c src/ring.c src/gfx.c src/font.c src/input.c src/lang.c $SRC -lpthread -lm"
gcc -O1 -g -Wno-format-truncation $SAN -DDRIVE="\"$H/fake/dev_hdd0\"" -DDEL_TEST_ROOT="\"$H/fake/dev_hdd0\"" -Isrc -o $H/shot $SHOT_SRC
gcc -O1 -g -Wno-format-truncation $SAN -DSCAN_QUIET_US=500000ULL -DSCAN_GIVE_UP_US=2000000ULL -DDRIVE="\"$H/fake/dev_hdd0\"" -DDEL_TEST_ROOT="\"$H/fake/dev_hdd0\"" -Isrc -o $H/shot_stuck $SHOT_SRC
export HOST_SAVE=$H HOST_DATA="$PWD/data" HOST_TOTAL=$((500*1024*1024*1024)) HOST_FREE=$((300*1024*1024*1024)) HOST_DISK=$((531*1024*1024*1024))
gcc -O1 -g -Wall -Wno-format-truncation -Wno-type-limits -Wno-format-overflow $SAN -Itools/host/mock -Isrc -o $H/probe_test tools/host/probe_test.c
$H/probe_test > $H/probe.log 2>&1 && tail -1 $H/probe.log
gcc -O1 -g -Wall -Wno-format-truncation $SAN -Isrc -o $H/games_test tools/host/games_test.c src/games.c src/iso.c src/sfo.c tools/host/fsx_host.c tools/host/plat_host.c -lpthread
$H/games_test > $H/games.log 2>&1; tail -1 $H/games.log
gcc -O1 -g -Wall -Wno-format-truncation $SAN -Isrc -o $H/health_test tools/host/health_test.c src/health.c tools/host/ata_host.c tools/host/plat_host.c -lpthread
HOST_SAVE=$H $H/health_test > $H/health.log 2>&1 && tail -1 $H/health.log
[ -f $H/tone.ogg ] || ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=1:sample_rate=48000" -ac 2 -c:a libvorbis $H/tone.ogg
gcc -O1 -g -Wno-format-truncation $SAN -Isrc -o $H/music_test tools/host/music_test.c src/music.c tools/host/plat_host.c -lpthread -lm
$H/music_test $H/tone.ogg data/music.ogg > $H/music.log 2>&1 && tail -1 $H/music.log
# every translation measured where it's drawn, at 4:3 (tools/check-fit.py)
gcc -O1 -Wno-format-truncation -Isrc -o $H/measure tools/host/measure.c src/font.c src/gfx.c src/fmt.c src/lang.c src/i18n.c src/settings.c tools/host/plat_host.c -lpthread -lm
python3 tools/check-fit.py > $H/fit.log 2>&1; tail -1 $H/fit.log
DT="$H/deltest/dev_hdd0"
gcc -O1 -g -Wall -Wno-format-truncation $SAN -DDEL_TEST_ROOT="\"$DT\"" -Isrc -o $H/del_test tools/host/del_test.c src/del.c src/bin.c src/settings.c \
	tools/host/del_host.c src/scan.c src/sfo.c src/iso.c tools/host/fsx_host.c tools/host/plat_host.c -lpthread
mkdir -p $H/deltest
HOST_DEL_ROOT="$DT" HOST_SAVE=$H/deltest $H/del_test > $H/del.log 2>&1 && tail -1 $H/del.log
$H/scan_test $H/fake/dev_hdd0 > $H/scan.log 2>&1 && tail -1 $H/scan.log
$H/scan_test $H/fake/dev_hdd0 60 > $H/cancel.log 2>&1 && tail -1 $H/cancel.log
$H/scan_test_cap $H/fake/dev_hdd0 > $H/cap.log 2>&1 && tail -1 $H/cap.log
$H/scan_test_dirs $H/fake/dev_hdd0 > $H/dirs.log 2>&1 && tail -1 $H/dirs.log
mkdir -p $H/stuck; HOST_SAVE=$H/stuck $H/scan_test_stuck $H/fake/dev_hdd0 stuck > $H/stuck.log 2>&1 && tail -1 $H/stuck.log
rm -rf $H/eio; mkdir -p $H/eio; HOST_SAVE=$H/eio $H/scan_test $H/fake/dev_hdd0 eio > $H/eio.log 2>&1 && tail -1 $H/eio.log
mkdir -p $H/folder; HOST_SAVE=$H/folder $H/scan_test $H/fake/dev_hdd0 folder > $H/folder.log 2>&1 && tail -1 $H/folder.log
mkdir -p $H/entries; HOST_SAVE=$H/entries $H/scan_test_entries $H/fake/dev_hdd0 entries > $H/entries.log 2>&1 && tail -1 $H/entries.log
HOST_SAVE=$H/entries $H/scan_test_exact $H/fake/dev_hdd0 entries > $H/exact.log 2>&1 && tail -1 $H/exact.log
# a drive with more than 64 folders of its own at the top (System & other): each partial rescan still matches the whole
M=$H/many
# (made again when the fake drive is: a copy of an older one hasn't what the tests now look for)
FV=$(ls -A $H/fake | grep '^\.v[0-9]' | head -1)		# (the fake drive's version: its marker's name)
[ -n "$FV" ] && [ -f "$M/.fake$FV" ] && [ -d $M/dev_hdd0/extra100 ] || { rm -rf $M && mkdir -p $M && cp -a $H/fake/dev_hdd0 $M/dev_hdd0 && for i in $(seq 1 100); do
	mkdir -p $M/dev_hdd0/extra$i/sub && head -c $((i * 100)) /dev/zero > $M/dev_hdd0/extra$i/sub/f.dat; done; touch "$M/.fake$FV"; }
# (and another game's things in FileSystem beside SingStar's songs: only FileSystem/Export is called that)
mkdir -p $M/dev_hdd0/FileSystem/OtherGame && head -c 5000 /dev/zero > $M/dev_hdd0/FileSystem/OtherGame/data.bin
HOST_SAVE=$M $H/scan_test $M/dev_hdd0 > $H/many.log 2>&1 && tail -1 $H/many.log
$H/scan_test $H/fake/dev_hdd0 > /dev/null 2>&1		# (the full cache again, for the shots)
$H/scan_test $H/fake/dev_usb000 plain > /dev/null 2>&1	# (and the USB stick's: cache_dev_usb000.bin, read when HOST_USB plugs it in)
# the screens: overview (16:9, 4:3 with Unaccounted picked), a category's items, a game's folder
# and one inside it, the largest items and files, a search, and the System partitions row
SHOTS="overview-1280:1280: overview-960:960:8 items-1280:1280:X items-960:960:X folder-1280:1280:X,X
	folder-deep-1280:1280:X,X,X,D,X saves-1280:1280:2,X largest-1280:1280:S largest-files-960:960:S,S
	search-1280:1280:X,T sort-name-960:960:X,SEL,SEL partitions-1280:1280:7
	health-ask-1280:1280:START,2,R,R,R,R,X health-1280:1280:START,2,R,R,R,R,X,X health-960:960:START,2,R,R,R,R,X,X
	usb-hints-960:960:+ usb-overview-1280:1280:+,R1
	delete-lock-1280:1280:START,3,R,X,8,X delete-hint-1280:1280:START,3,R,X,8,X,X,O,O,1,X delete-confirm-1280:1280:START,3,R,X,8,X,X,O,O,1,X,START,R
	delete-holding-960:960:START,3,R,X,8,X,X,O,O,1,X,START,R,HELD1200 delete-refused-1280:1280:START,3,R,X,8,X,X,O,O,1,X,START,R,HOLD2300
	delete-blocked-1280:1280:START,3,R,X,8,X,X,O,O,X,START
	menu-1280:1280:START settings-1280:1280:START,3,R,X settings-960:960:START,3,R,X,D,R
	about-1280:1280:START,3,R,R,R,X export-960:960:START,2,R,R,R,X changes-1280:1280:START,2,X
	free-1280:1280:START,X games-1280:1280:START,R,X dupes-1280:1280:START,R,R,X game-960:960:START,R,R,X,X
	types-1280:1280:START,1,R,R,X types-iso-1280:1280:START,1,R,R,X,R1,X buttons-1280:1280:START,3,R,R,X buttons-960:960:START,3,R,R,X,D,D,D,D,D,D,D,D,D,D,D,D,D,D,D,D,D,D,D
	rescan-items-1280:1280:X,S
	folders-1280:1280:START,1,X old-1280:1280:START,R,R,R,R,X map-1280:1280:START,1,R,R,R,X map-in-960:960:START,1,R,R,R,X,X
	history-1280:1280:START,2,R,X marks-1280:1280:X,X,S,O,O,START,1,R,R,R,R,X health-hist-1280:1280:START,2,R,R,R,R,X,X,S health-help-1280:1280:START,2,R,R,R,R,X,X,SEL health-help-960:960:START,2,R,R,R,R,X,X,SEL,D,D,D,D,D,D,D,D,D,D,D,D
	dupes2-1280:1280:+,START,R,R,X"
PNGS=""
for s in $SHOTS; do
	n=${s%%:*}; r=${s#*:}; w=${r%%:*}; k=${r#*:}
	rm -f $H/smart_journal.txt $H/settings.txt $H/deletions.log $H/language.txt $H/bookmarks.txt
	usb=""; case "$k" in +*) usb=1; k=${k#+}; k=${k#,};; esac		# (+: a USB drive plugged in)
	case "$n" in delete-refused-*) printf 'bin=0\n' > $H/settings.txt;; esac		# (a delete for good, refused: the recycle bin off)
	HOST_USB=$usb HOST_QUERY="the last" $H/shot $H/$n.ppm $w "$k" > /dev/null
	PNGS="$PNGS $n"
done
# mid-scan (a slow scan): the hints must stop short of the strip
HOST_SLOW=1 HOST_USB=1 $H/shot $H/scanning-960.ppm 960 "" 400 > /dev/null
HOST_SLOW=1 $H/shot $H/scanning-largest-960.ppm 960 "T,S" > /dev/null
HOST_SLOW=1 HOST_AFTER="START" $H/shot $H/scanning-start-1280.ppm 1280 "" 300 > /dev/null	# (START while scanning: a note, no menu)
# Rescan at start, with the free space moved: the start screen while it scans (Tools off till it's done)
printf 'autoscan=1\n' > $H/settings.txt
HOST_SLOW=1 HOST_HOME=1 HOST_FREE=$((290*1024*1024*1024)) $H/shot $H/scanning-home-960.ppm 960 "R,R" > /dev/null
rm -f $H/settings.txt
# another stick in usb000 (another size): its port's cached scan isn't of it, so it's "No scan of USB 000 yet"
HOST_USB=1 HOST_USB_TOTAL=$((32*1024*1024*1024)) $H/shot $H/usb-other-1280.ppm 1280 "R1" > $H/usb-other.log
grep -q "the cached scan's of another drive" $H/usb-other.log && echo "PASS (another stick in the port: its cached scan isn't shown)"
# a console that confirms with O (Japan, Asia): the delete lock says to hold Circle, as its hint shows
rm -f $H/settings.txt
HOST_O_CONFIRM=1 $H/shot $H/delete-lock-o-1280.ppm 1280 "START,3,R,X,8,X" > /dev/null
rm -f $H/settings.txt
# a drive that never answers in home/: the scan given up on (here after 2 s, not 5 minutes), and what to do
HOST_STUCK=/home $H/shot_stuck $H/stuck-1280.ppm 1280 "" 3000 > /dev/null
HOST_STUCK=/home $H/shot_stuck $H/stuck-960.ppm 960 "" 3000 > /dev/null
echo de > $H/language.txt; HOST_STUCK=/home $H/shot_stuck $H/stuck-de-960.ppm 960 "" 3000 > /dev/null; rm -f $H/language.txt
PNGS="$PNGS stuck-1280 stuck-960 stuck-de-960"
# the internal drive's read errors (bad sectors): home/ skipped, and the top unreadable (the scan didn't finish), each pointing
# to Drive health (their own save folder: the scan with home/ skipped isn't the one the other shots show)
E=$H/eio; rm -rf $E; mkdir -p $E; cp $H/cache_dev_hdd0.bin $E/
HOST_SAVE=$E HOST_EIO=/home $H/shot $H/read-errors-1280.ppm 1280 "" 1500 > $E/shot.log
grep -q "gave 1 read errors: Drive health suggested" $E/shot.log && echo "PASS (read errors on the internal drive: Drive health suggested)"
cp $H/cache_dev_hdd0.bin $E/; echo de > $E/language.txt
HOST_SAVE=$E HOST_EIO=/home $H/shot $H/read-errors-de-960.ppm 960 "" 1500 > /dev/null
cp $H/cache_dev_hdd0.bin $E/; rm -f $E/language.txt
HOST_SAVE=$E HOST_EIO=/fake/dev_hdd0 $H/shot $H/read-unfinished-960.ppm 960 "" 1500 > $E/shot.log
grep -q "and the scan didn't finish: Drive health suggested" $E/shot.log && echo "PASS (the internal drive unreadable: the scan didn't finish, Drive health suggested)"
PNGS="$PNGS read-errors-1280 read-errors-de-960 read-unfinished-960"
# an SSD's health: its life left, as new, worn (in German at 4:3: the longest verdict), and its readings over time
rm -f $H/smart_journal.txt
HOST_ATA=ssd $H/shot $H/health-ssd-1280.ppm 1280 "START,2,R,R,R,R,X,X" > /dev/null
rm -f $H/smart_journal.txt; echo de > $H/language.txt
HOST_ATA=ssd-worn $H/shot $H/health-ssd-worn-de-960.ppm 960 "START,2,R,R,R,R,X,X" > /dev/null
rm -f $H/smart_journal.txt $H/language.txt
HOST_ATA=ssd $H/shot $H/health-ssd-hist-1280.ppm 1280 "START,2,R,R,R,R,X,X,S" > /dev/null
rm -f $H/smart_journal.txt
PNGS="$PNGS health-ssd-1280 health-ssd-worn-de-960 health-ssd-hist-1280"
# a worn drive: X on its reading, what to do (and in German at 4:3, scrolled to the end)
HOST_ATA=worn $H/shot $H/health-todo-1280.ppm 1280 "START,2,R,R,R,R,X,X,X" > /dev/null
rm -f $H/smart_journal.txt; echo de > $H/language.txt
HOST_ATA=worn $H/shot $H/health-todo-de-960.ppm 960 "START,2,R,R,R,R,X,X,X,D,D,D,D,D,D,D,D,D,D,D,D" > /dev/null
rm -f $H/smart_journal.txt $H/language.txt
PNGS="$PNGS health-todo-1280 health-todo-de-960"
PNGS="$PNGS scanning-960 scanning-largest-960 scanning-start-1280 scanning-home-960 usb-other-1280 delete-lock-o-1280"
# the intro a second in, and the start screen: with the last scan, 4:3 with a USB drive (Scan picked), and with none yet
HOST_INTRO=1000 $H/shot $H/intro-1280.ppm 1280 > /dev/null
HOST_HOME=1 $H/shot $H/home-1280.ppm 1280 "" > /dev/null
HOST_HOME=1 HOST_USB=1 $H/shot $H/home-960.ppm 960 "L" > /dev/null
rm -rf $H/nocache; mkdir -p $H/nocache
HOST_SAVE=$H/nocache HOST_HOME=1 $H/shot $H/home-first-1280.ppm 1280 "" > /dev/null
PNGS="$PNGS intro-1280 home-1280 home-960 home-first-1280"
# 0.9: Size filter, Identical files (a long look: waited for), a game's parts, Free up space with a kept row, Settings' new rows
HOST_WAIT=300 $H/shot $H/filter-1280.ppm 1280 "START,1,R,X" > /dev/null
HOST_WAIT=30000 $H/shot $H/same-1280.ppm 1280 "START,R,R,R,X" > /dev/null
HOST_WAIT=1500 $H/shot $H/parts-960.ppm 960 "START,R,X,X,S" > /dev/null
$H/shot $H/keep-1280.ppm 1280 "START,X,S" > /dev/null
rm -f $H/keep.txt
$H/shot $H/settings2-1280.ppm 1280 "START,3,R,X,4,R" > /dev/null
rm -f $H/settings.txt
$H/shot $H/menu-960.ppm 960 "START" > /dev/null
# 0.9.1: Recently added (Square: back a month to 3 months), and in German at 4:3
$H/shot $H/recent-1280.ppm 1280 "START,2,R,R,X,S" > /dev/null
echo de > $H/language.txt; $H/shot $H/recent-de-960.ppm 960 "START,2,R,R,X" > /dev/null; rm -f $H/language.txt
PNGS="$PNGS filter-1280 same-1280 parts-960 keep-1280 settings2-1280 menu-960 recent-1280 recent-de-960"
python3 -c "
from PIL import Image
import sys
for n in sys.argv[1:]: Image.open('$H/' + n + '.ppm').save('$H/' + n + '.png')
print('shots: $H/{' + ','.join(sys.argv[1:]) + '}.png')" $PNGS
