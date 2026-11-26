# HDD Atlas 1.0.0

A PS3 homebrew app that shows what's using your hard drive's space (and your USB drives', FAT32 or NTFS), and helps you free some of it safely.

Title ID `HDAT00001`. For PS3HEN and CFW. Licence: GPL-2.0.

## Install

Install `HDDAtlas-v1.0.0.pkg` from the XMB (Game > Install Package Files, or through webMAN). Nothing's scanned until you choose Scan on the start screen. The last scan of each drive is kept, so the app reopens instantly.

## What it does

- **See where the space went.** A ring chart and list by category: installed games, disc backups, saves/trophies/licences, themes, media, packages and caches, system folders, the system's own partitions, and the reserved space the file system keeps (X on it says what it is). Open any category, the largest items and files, or any folder down to the last file.
- **Games, all together.** Each game's install, backups, saves and trophies added up by title ID, with titles and icons read from PARAM.SFO and from inside disc images.
- **Free up space.** What's likely safe to clear, each row with why. Rows that need a look first are shown in yellow.
  - leftover and not-yet-installed packages (checked against what's installed)
  - webMAN's cache, and other apps' caches (IRISMAN's, Apollo's, multiMAN's covers)
  - extra copies of the same game
  - installs and deletes the system didn't finish
  - downloads and queued installs
  - system updates (not for HEN or CFW)
  - updates and saves for games that aren't on the drive
- **Find things.** Duplicates (also across drives), identical files (byte for byte), a size filter, recently added, not played lately, largest folders, file types, a map of the drive, bookmarks, search.
- **Over time.** Changes since the last scan, space over time, and a report (text and CSV) to the HDD or a USB drive.
- **NTFS drives.** The PS3 only reads FAT32 drives. HDD Atlas mounts NTFS ones itself, shown as NTFS 0-7: scanned, deleted from and with a recycle bin, like any USB drive.
- **Drive health.** The internal HDD's SMART data, read-only, with each reading kept and a counter that grew pointed out.
- **16 languages**, following the PS3's by default. Music (R3 turns it off), sound effects, accent colours.

## Deleting: off until you turn it on

Deleting is off by default (Settings turns it on). Then START on a row deletes it, one at a time, after you hold X for 2 seconds. Every delete is logged in `USRDIR/deletions.log`.

- **Only these can go:**
  - disc images
  - folder backups in GAMES
  - leftover packages
  - ROMs
  - webMAN's cache
  - installs the system left unfinished
  - system update files
  - entries of the recycle bin
- **Kept for webMAN:** its and prepISO's stubs for games on NTFS drives (`tmp/wmtmp/*.ntfs[...]`) and disc images' decryption keys (`.dkey`), though they're in webMAN's cache or beside a disc image: webMAN needs them to list and mount those games.
- **Never from HDD Atlas:** installed games, saves, trophies, music, video and photos. The XMB keeps lists of them, so delete them there. System folders are never touched.
- **The recycle bin** is on by default. A delete is a move into the bin on the same drive, kept 7 days (Settings changes this) and put back with X. Space is freed when the bin's emptied.
- **Links** (symbolic links) are never followed: the scan doesn't count what they point at, and a delete leaves them.
- **Identical files** never deletes the last copy, and checks a file against the copy that stays, byte for byte, first.
- **An unfinished install** (`game/_INST_…`) isn't offered for a day after it last changed. It's checked again just before it goes, and it's refused while PS3HEN is installing a package.

## Known limits

- **NTFS drives Windows didn't close cleanly** (pulled out without Safely Remove, or Windows' Fast Startup or hibernation) are read-only: scanned, but nothing deleted. Safely remove it on a PC (or turn Fast Startup off), then plug it in again. Only a drive's first NTFS partition is read, a report isn't saved to an NTFS drive, and exFAT isn't read at all.
- **Writing to NTFS** goes through libntfs_ext, whose author calls its write support experimental. On the drives it was tested with (moves into the bin, put back, deleted for good), Windows' chkdsk found no problems, but keep a backup of what matters. Quit HDD Atlas before unplugging an NTFS drive: quitting closes it cleanly.
- **webMAN's hard links.** No PS3 file call reports links, so a file webMAN linked rather than copied is counted twice in the totals, and deleting one name frees nothing. A report says so when the files add up to more than the used space.
- **App caches are shown, not deleted.** Apart from webMAN's, apps' caches are pointed out, to clear in the app that made them.
- **The internet system-update download** is stored somewhere no source has found yet, so it isn't recognised. Update files in `updater/` and on disc backups are.

## Credits

Made by daedra000 and Claude (Anthropic).

- **Hard disk icon:** "Internal Clear" by Rick Patrick (SoftIcons).
- **Menu icons:** Tanah Basah (Flaticon).
- **Music:** "Relaxing Music (Part 1)" by Piotr Witowski (Pixabay).
- **Libraries and fonts:** stb_vorbis (public domain), PSL1GHT, libntfs_ext by Estwald (from NTFS-3G, GPL-2.0), the Poppins and DejaVu fonts, and flag-icons.

The full licences are in `USRDIR/data/licenses` and `USRDIR/data/fonts`, and the credits in the app's About.

## Source

HDD Atlas is free software under the GPL-2.0, and so is libntfs_ext inside it. The source is `HDDAtlas-v1.0.0-source.zip`, beside the package: `sh tools/build.sh` builds it (README.md says how).
