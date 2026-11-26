# Mapping the PS3 drive behind HDD Atlas

A PS3 hard drive is a stack of regions, and HDD Atlas scans only one of them. The stack is a 4 KB partition table, then a 256 MB VFLASH region on NOR consoles, then the UFS2 `/dev_hdd0` that HDD Atlas walks, then a fixed 2 GB FAT16 `/dev_hdd1` game cache, and on old installs an OtherOS region. Inside `/dev_hdd0` there are two layers. Sony owns `game/<TITLEID>`, `home/<user>/{savedata,trophy,exdata}`, the media folders and a set of `vsh`/`mms`/`tmp`/`updater` folders. webMAN MOD, PS3HEN, IRISMAN, Apollo and multiMAN add backup folders, caches, plugins and config on top, and PS3HEN recreates 14 default folders at every boot. HDD Atlas's design already avoids the usual traps (iterative scan, read-only, CATEGORY-aware, an allow-list for deletes), so the open problems are in accounting and labelling:

- **The stock 8% free-space reserve**, which webMAN can cut to 1%, will show up as "unaccounted" space or as a sudden jump in free space.
- **webMAN's hard links between `GAMES` and `game/<ID>`** cannot be seen through PSL1GHT's stat. They get counted twice and inflate the "identical files" and "extra copies" findings.
- **The two outputs of syscall 840 are described differently** by psdevwiki and by webMAN's source.
- **`/dev_hdd0/video` can hold webMAN "stealth" ISOs.**
- **`/dev_hdd0/exdata` holds HEN's RAP licences.**
- **"webMAN's cache" in the delete allow-list needs a tighter definition.** It must leave out `tmp/wm_res`, which holds a user-supplied PS1 BIOS, and it should treat `.ntfs[...]` stubs as necessary for NTFS/exFAT games, not as junk.

Several new "free up space" candidates are backed by source code: IRISMAN's joined-file cache (gigabytes per title), the Apollo self-update PKG that Apollo never deletes, webMAN's `~tmp.iso` and `tmp/extract`, and `PS3_UPDATE` folders inside folder backups. Many primary pages could not be reached during research, including psdevwiki's PARAM.SFO, Dev_hdd0, Partitions and mount-point pages and all of psx-place. Much of Sony's own folder map is therefore marked [unverified] below, and the report ends with a checklist to confirm on your HEN and CFW consoles.

## Raw capacity loses five slices before a single file is counted

The best-documented part of the drive is its partition layout. psdevwiki's (self-described "experimental") HDD table gives these regions, using their Linux names:

- **ps3da**: a **4 KB partition table**.
- **ps3vflash**: a **256 MB VFLASH region (0x80000 sectors), on NOR models only**.
- **ps3db**: **dev_hdd0 (UFS2, the rest of the disk)**.
- **ps3dc**: **dev_hdd1 (FAT16, exactly 0x7FFFF000 bytes, 4,194,296 sectors)**.
- **ps3dd**: an optional OtherOS region, which firmware 3.21 disabled.

The usual gap between regions is 0x8 sectors, so **dev_hdd0 starts at sector 0x18 on NAND models and 0x80018 on NOR models** ([psdevwiki Talk:Harddrive](https://psdevwiki.com/ps3/Talk:Harddrive)). psdevwiki describes the GameOS partition as UFS2 with 512-byte sectors ([psdevwiki Harddrive](https://www.psdevwiki.com/ps3/Harddrive)). A decrypted 320 GB Slim identified as big-endian UFS2 with **16,384-byte blocks and 2,048-byte fragments**, mounted internally at `/cell_mw_cfs` ([Woolfy025 gist](https://gist.github.com/Woolfy025/6b275e06b166fffd50e059f4198e1aa8)).

On NOR consoles, VFLASH holds dev_flash (FAT16, 199.75 MB), dev_flash2 (FAT16, 16 MB, the xRegistry) and dev_flash3 (FAT12, 512 KB, the CRL/DRL revocation lists), plus raw areas and about 16.25 MB of padding ([psdevwiki Talk:Harddrive](https://psdevwiki.com/ps3/Talk:Harddrive)). On NAND consoles the same three regions live in 256 MB of real flash and not on the HDD ([psdevwiki Flash mirror](https://backiee.wasmer.app/https_www_psdevwiki_com/ps3/Flash)). Which model is which was not confirmed from a fetched source. From general knowledge [unverified], CECHA/B/C/E/G are NAND and everything from CECHH onward is NOR, including the CECHK fat and every Slim and Super Slim. That puts the CFW test console in the VFLASH-on-HDD group.

| Region / mount | Filesystem | Size | Present on | What it holds | Count it as | Source |
|---|---|---|---|---|---|---|
| Partition table (ps3da) | — | 4 KB | All | Region table | System's own partitions | [Talk:Harddrive](https://psdevwiki.com/ps3/Talk:Harddrive) |
| VFLASH (ps3vflash) | raw + FAT16/FAT12 inside | 256 MB | NOR only | dev_flash, dev_flash2, dev_flash3, OtherOS bootloader area | System's own partitions (do not add dev_flash* on top) | [Talk:Harddrive](https://psdevwiki.com/ps3/Talk:Harddrive) |
| `/dev_hdd0` (ps3db) | UFS2, big-endian | Rest of disk | All | Everything HDD Atlas scans | Scanned categories + reserve + metadata | [Harddrive](https://www.psdevwiki.com/ps3/Harddrive), [gist](https://gist.github.com/Woolfy025/6b275e06b166fffd50e059f4198e1aa8) |
| `/dev_hdd1` (ps3dc) | FAT16 | 2 GB fixed | All | System/game cache | System's own partitions (fixed size, contents irrelevant) | [Talk:Harddrive](https://psdevwiki.com/ps3/Talk:Harddrive) |
| OtherOS (ps3dd, "dev_hdd2") | Linux fs | User-chosen (10 GB example) | Drives set up on firmware 3.15 or earlier, or OtherOS++ | Linux | System's own partitions | [Talk:Harddrive](https://psdevwiki.com/ps3/Talk:Harddrive) |
| `/dev_flash`, `/dev_flash2`, `/dev_flash3` | FAT16/FAT16/FAT12 | 199.75 MB / 16 MB / 512 KB | All (inside VFLASH on NOR) | Firmware, xRegistry, CRL/DRL | Never add to HDD totals separately | [Flash mirror](https://backiee.wasmer.app/https_www_psdevwiki_com/ps3/Flash) |
| `/dev_bdvd`, `/app_home` | view | — | All | Disc, or `game/<ID>` remapped while a game runs | Do not count (double counting) | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers) |
| `/dev_usb000…` | FAT (4 GB file limit) | Drive | All | External storage | Separate drive | [psdevwiki Hdd](https://www.psdevwiki.com/ps3/Hdd) |
| `/dev_ms`, `/dev_cf`, `/dev_sd` | FAT | Card | Fat models with card readers | Memory cards | Separate drive | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers) |
| `/dev_ps2disc` | view | — | webMAN | Mounted PS2 image | Do not count | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers) |
| `/dev_blind` | FAT (dev_flash remounted read-write) | — | Created by tools | Writable view of dev_flash | Do not count; never mount automatically | Mount call in [webMAN source](https://github.com/aldostools/webMAN-MOD); other details [unverified] |
| `ntfs*`, `net*` | libntfs / ps3netsrv | — | webMAN, IRISMAN | NTFS/exFAT USB drives, network shares | Not GameOS mounts; only visible if HDD Atlas links libntfs | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers) |
| `/host_root` | host PC | — | Dev kits, emulator | Host filesystem | Ignore | [unverified] |

The space that goes missing between the label on the drive and the XMB's free figure comes from three sources. **Sony reserves part of the storage "for system administration" and says that space "is not otherwise available for your use"**, and since firmware 1.10 the XMB reports capacity in binary units ([PS3 User's Guide: System Information](https://ps3-pro.github.io/Guides/manuals/en/ps3/current/settings/systeminfo.html)). The partition structure takes its fixed slices. And, most importantly for HDD Atlas, there is a percentage reserve inside dev_hdd0: **webMAN's `/unlockhdd.ps3?enable` "sets reserved HDD space to 1% (unlocks 7%)" and `?disable` restores 8%**. It needs Cobra and `sys_admin`, and it reports the new minimum-free percentage and optimisation status ([webMAN wiki: Web Commands](https://github.com/aldostools/webMAN-MOD/wiki/Web-Commands); [webMAN source, include/cmd/unlockhdd.h](https://github.com/aldostools/webMAN-MOD)). Community reports put the total OS and formatting loss at around 10% on a 931 GB drive ([r/ps3hacks mirror](https://reddit.birdcat.cafe/r/ps3hacks/comments/1ipizm2/genuine_question_why_dont_i_get_828gb_out_of_931)).

For scale: 8% of a 500 GB drive is about 37 GiB. On most consoles that is bigger than any category except games and backups. It is also not "unaccounted" in any useful sense, since it is a known, adjustable reserve.

The waterfall below is the accounting model HDD Atlas can show without guessing. Each line can be measured or derived:

| Slice | How to get it | Size |
|---|---|---|
| Raw drive capacity | ATA IDENTIFY sector count × 512 (the SMART code in `ata_ps3.c` already talks to the drive; an HPA-clamped drive reports its clamped size) | Drive |
| − Partition table + VFLASH + gaps | Fixed: 0x18 sectors (NAND) or 0x80018 sectors (NOR) before dev_hdd0 | 12 KB or about 256 MB |
| − dev_hdd1 | Fixed 0x7FFFF000 bytes | 2 GiB |
| − OtherOS (rare) | Whatever is left after dev_hdd0 and dev_hdd1 | 0 on almost every console |
| = dev_hdd0 partition | | |
| − UFS2 metadata (inodes, cylinder groups) | Partition size minus the filesystem total reported by syscall 840 | Inferred; typically a few % |
| − Minimum-free reserve | 8% stock, 1% after webMAN unlockhdd (difference between the two syscall-840 outputs if psdevwiki's naming is right) | 8% or 1% |
| − Used | Filesystem total − free | |
| − Sum of scanned file sizes | HDD Atlas's scan | |
| = Truly unaccounted | Fragment rounding, directory blocks, unreadable folders, **minus** hard-link double counting | Should be small; a negative value means double counting |

psdevwiki's talk page describes a 2 TB drive clamped with HPA to about 1.73 TB, with warnings of errors at 1.75 TB on firmware 4.46 ([psdevwiki Talk:Harddrive](https://psdevwiki.com/ps3/Talk:Harddrive)). On such drives, IDENTIFY's current maximum LBA is the figure to use, not the native size.

## /dev_hdd0 splits into a Sony layer and a homebrew layer

### Sony's folders

The Sony layer is the least well sourced part of this research, because psdevwiki's Dev_hdd0 page could not be fetched. These folders are confirmed from tool documentation and source: `game/` (installed PSN games, mapped to `/dev_bdvd` during play), `home/<8-digit user>/savedata`, `home/<user>/exdata` (`*.rif` licences), `home/<user>/ps2emu2_savedata` (PS2 Classics VME cards), `savedata/vmc` (PS1 VM1 and PS2 VM2 cards) and the trophy tree ([webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers); [GameBrew: Apollo](https://www.gamebrew.org/wiki/Apollo_Save_Tool_PS3); [psdevwiki Trophy files](https://psdevwiki.com/ps3/Trophy_files)). RPCS3's loader requires user folders to be 8-digit numeric names ([RPCS3 system_utils.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/system_utils.cpp)). Everything else in the table rests on background knowledge and is marked as such.

| Path | Purpose | Creator | Typical size | Suggested Atlas category | Status |
|---|---|---|---|---|---|
| `game/<TITLEID>/` | Installed PSN games (HG), disc-game data and patches (GD), classics, homebrew apps | Sony installer / PKG | 100 MB – 40+ GB for PSN games; patches from tens of MB to several GB | Games, split by CATEGORY | Path cited ([webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers)); sizes [unverified] |
| `game/<ID>/C00/` | Content used when a trial (HG) has been unlocked to the full game | Sony | — | Games (same title) | [RPCS3 system_utils.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/system_utils.cpp) |
| `game/<ID>/USRDIR/*.edat, *.sdat` | DLC and encrypted data | Sony | KB – GB | Games (same title) | Format cited ([EDAT](https://psdevwiki.com/ps3/EDAT_files)); location [unverified] |
| `game/_INST_*`, `game/_DEL_*` | Install or delete in progress (README's names) | Sony | Any | Unfinished installs | **Not found in any source**; verify |
| `home/<u>/savedata/<SAVEDATA_DIRECTORY>/` | PS3 saves: PARAM.SFO (SD), PARAM.PFD, ICON0, data | Sony | KB to tens of MB; some games 100+ MB | Saves | Path cited ([Apollo](https://www.gamebrew.org/wiki/Apollo_Save_Tool_PS3), [PFD](https://psdevwiki.com/ps3/PARAM.PFD)); sizes [unverified] |
| `home/<u>/trophy/<NPWR#####_00>/` | Installed trophy set: TROPCONF.SFM, TROP*.SFM, TROPUSR.DAT, TROPTRNS.DAT (encrypted), PNGs, PARAM.SFO, PARAM.PFD | Sony | 100 KB to a few MB per game | Trophies (never delete) | Layout cited ([Trophy files](https://psdevwiki.com/ps3/Trophy_files)); sizes [unverified] |
| `home/<u>/trophy/_TROPSYS_/` | TROPSYS.DAT index of the user's trophy installs, plus PARAM.PFD | Sony | Small | Trophies | [Trophy files](https://psdevwiki.com/ps3/Trophy_files), [PFD](https://psdevwiki.com/ps3/PARAM.PFD) |
| `home/<u>/trophy/_BU_*` | Previous trophy folder, renamed when a trophy set is upgraded | Sony | Small | Trophies, flagged "old copy" | Rename cited ([Trophy files](https://psdevwiki.com/ps3/Trophy_files)); whether it lingers [unverified] |
| `home/<u>/exdata/` | `*.rif` licences, `act.dat`; RPCS3 also reads `<CONTENT_ID>.rap` and unlock `.edat` here | Sony / CFW | Under 5 KB each | Licences (never delete) | [Apollo](https://www.gamebrew.org/wiki/Apollo_Save_Tool_PS3), [RPCS3](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/system_utils.cpp); sizes [unverified] |
| `home/<u>/ps2emu2_savedata/` | PS2 Classics VME cards | Sony | MB | Saves | [Apollo](https://www.gamebrew.org/wiki/Apollo_Save_Tool_PS3) |
| `home/<u>/webbrowser/history.xml` | Browser history (HEN can clear it) | Sony | Small | System | [PS3HEN source](https://github.com/PS3Xploit/PS3HEN) |
| `home/<u>/friendim/`, `minis_savedata/`, `localusername`, `np/` | Friends/IM cache, minis saves, profile name, PSN data | Sony | Small | Saves (minis); System (rest) | [unverified] |
| `savedata/vmc/` | PS1 VM1 and PS2 VM2 virtual memory cards (e.g. `Internal_MC.VM1`) | Sony / IRISMAN | MB | Saves | [Apollo](https://www.gamebrew.org/wiki/Apollo_Save_Tool_PS3), [IRISMAN source](https://github.com/aldostools/IRISMAN) |
| `savedata/vmu/` | Dreamcast VMU cards | Apollo convention | KB | Saves | [Apollo](https://www.gamebrew.org/wiki/Apollo_Save_Tool_PS3) |
| `music/`, `photo/`, `video/` | XMB-imported media in numbered subfolders | Sony | GB to tens of GB | Media (but see the `video` ISO caveat) | [unverified]; `video` as an ISO folder is cited |
| `mms/` | Media server database (`db/`) | Sony | Tens to hundreds of MB | System | [unverified] |
| `vsh/` (`task.dat`, `game_pkg/`, `pushlist/`) | XMB download queue; `game_pkg` holds downloaded PKGs waiting to install | Sony | `game_pkg` can be multi-GB | Packages/tmp (pending) | Paths referenced in [IRISMAN](https://github.com/aldostools/IRISMAN)/[webMAN](https://github.com/aldostools/webMAN-MOD) source; meaning [unverified] |
| `updater/` | Firmware update staging (PS3UPDAT.PUP) | Sony; HEN creates `updater/01` | About 200 MB when present | Packages/tmp | Creation cited ([PS3HEN](https://github.com/PS3Xploit/PS3HEN)); contents [unverified] |
| `theme/` | Installed `.p3t` themes | Sony; HEN creates the folder | 1 MB to tens of MB each | Themes | Folder cited ([PS3HEN](https://github.com/PS3Xploit/PS3HEN)); `.p3t` storage [unverified] |
| `tmp/` | Shared temp for system and homebrew | Sony + everyone | Varies | Split by subfolder (below) | Cited as a homebrew cache root |
| `drm/`, `crash_report/`, `data/`, `disc/`, `hdd/`, `etc/`, `kernel/` | Video DRM, crash dumps, other system data | Sony | Small | System | [unverified] |

### Folders added by CFW, HEN and homebrew

The homebrew layer is well sourced, because webMAN, PS3HEN, IRISMAN and Apollo are open source and their paths were read straight from current code. **PS3HEN's `create_default_dirs()` runs at every HEN start and creates `/dev_hdd0/BDISO`, `DVDISO`, `GAMES`, `PS2ISO`, `PS3ISO`, `PSPISO`, `PSXISO`, `ROMS`, `exdata`, `packages`, `plugins`, `theme`, `updater` and `updater/01` if they are missing** ([PS3HEN source](https://github.com/PS3Xploit/PS3HEN)). The HEN v3.4.0 changelog describes this as creating "standard folders if missing on dev_hdd0" ([GameBrew: PS3 HEN](https://www.gamebrew.org/wiki/PS3_HEN)).

webMAN's official uninstall page is the most complete list of its own footprint ([webMAN wiki: Uninstalling](https://github.com/aldostools/webMAN-MOD/wiki/~-Uninstalling-webMAN-MOD)). Its scan paths and split-file rules come from the Game Paths page: split ISOs are `.iso.0` to `.iso.63` at 4 GB or less each ([webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers)), and IRISMAN treats `.666` plus two digits (`.66600`, `.66601`…) as split big files inside folder backups ([IRISMAN source](https://github.com/aldostools/IRISMAN)).

| Path | Purpose | Creator | Typical size | Suggested Atlas category | Source |
|---|---|---|---|---|---|
| `GAMES/`, `GAMEZ/` | Folder (JB) backups: `PS3_DISC.SFB` + `PS3_GAME/`, `.666xx` splits, optional `covers/` | multiMAN, IRISMAN, webMAN; HEN creates `GAMES` | 1–50 GB each [unverified] | Disc backups | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers), [ConsoleMods multiMAN](https://consolemods.org/wiki/PS3:MultiMAN) |
| `PS3ISO/` | `.iso`, `.iso.0–.63`, `.key`/`.dkey`; transient `~tmp.iso` | webMAN etc.; HEN | Up to BD-50 size [unverified] | Disc backups | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers), [webMAN source](https://github.com/aldostools/webMAN-MOD) |
| `video/` (ISOs) | webMAN's "stealth" PS3ISO folder | User / webMAN | Multi-GB | **Disc backups when the file is a PS3 ISO**, Media otherwise | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers) |
| `PS2ISO/` | `.iso` + `.CONFIG`; PS2 Classics `.BIN.ENC` with `.BIN.ENC.PNG`/`PIC0-2` art; **copies of PS2 ISOs from USB/NTFS/net made for play** | webMAN; HEN | Up to DVD-9 size [unverified] | Disc backups | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers) |
| `PSXISO/`, `PSXGAMES/` | `.cue/.bin`, `.img/.ccd`, `.iso`, `.mdf`; IRISMAN `PSXGAMES/<name>/disc%i.img`, `CHEATS` | webMAN, IRISMAN | About 700 MB each [unverified] | Disc backups | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers), [IRISMAN](https://github.com/aldostools/IRISMAN) |
| `PSPISO/`, `BDISO/`, `DVDISO/`, `PS2DISC/` | PSP ISOs, Blu-ray and DVD video images, extracted SingStar PS2 discs | webMAN; HEN creates the first three | GB | Disc backups | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers) |
| `ROMS/<system>/` | RetroArch ROMs, optional `gamelist.txt` | User; HEN creates | Varies | ROMs | [webMAN wiki: ROMS](https://github.com/aldostools/webMAN-MOD/wiki/ROMS-Support) |
| `packages/` | PKGs waiting to install; Apollo self-update `apollo-ps3.pkg` | User, webMAN, Apollo; HEN | MB to GB | Packages | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers), [apollo-ps3](https://github.com/bucanero/apollo-ps3) |
| `exdata/` (root) | **HEN/CFW RAP licences**, `rap.bin` | HEN creates | Tiny | **Licences (never delete)** | [PS3HEN](https://github.com/PS3Xploit/PS3HEN) |
| `plugins/` (+ `kernel/`, `ps3xpad/`, `ps3_menu/`, `PS3Lock/`, `images/`) | VSH plugins (`webftp_server*.sprx`, `psnpatch.sprx`) | webMAN, user; HEN | Small | CFW/HEN tools | [webMAN uninstall](https://github.com/aldostools/webMAN-MOD/wiki/~-Uninstalling-webMAN-MOD), [GameBrew HEN](https://www.gamebrew.org/wiki/PS3_HEN) |
| `boot_plugins*.txt`, `boot_plugins*.hen`, `boot_init.txt`, `autoexec.bat`, `ingame.bat`, `onxmb.bat` | Plugin load lists and scripts | Cobra/webMAN/HEN | Bytes | CFW/HEN tools (never delete) | [webMAN uninstall](https://github.com/aldostools/webMAN-MOD/wiki/~-Uninstalling-webMAN-MOD), [PS3HEN](https://github.com/PS3Xploit/PS3HEN) |
| `hen/xml/`, `hen/toggles/*.on`, `*.off`, `hen_updater.off`, `hen.test*`, `log_file.bin` | HEN settings, XML and flags | PS3HEN | Small | CFW/HEN tools | [PS3HEN](https://github.com/PS3Xploit/PS3HEN) |
| `vm/cobra_cfg.bin` | Cobra configuration | Cobra/HEN | Bytes | CFW/HEN tools (never delete) | [PS3HEN](https://github.com/PS3Xploit/PS3HEN) |
| `xmlhost/game_plugin/` (`mygames.xml`) | webMAN's XMB game list | webMAN | Small | CFW/HEN tools | [webMAN source](https://github.com/aldostools/webMAN-MOD) |
| `classic_ps2` | webMAN PS2 Classics file | webMAN | [unverified] | CFW/HEN tools | [webMAN uninstall](https://github.com/aldostools/webMAN-MOD/wiki/~-Uninstalling-webMAN-MOD) |
| `tmp/wmtmp/` | Cover cache, **`.ntfs[...]` stubs for NTFS/exFAT ISOs**, `last_games.bin`, `games.html`, `filelist.txt`, `slaunch.cfg`, `~netpsx.cue`; IRISMAN writes `.ntfs[...]`, `.PNG/.jpg/.SFO/.iso` here too | webMAN, IRISMAN | Tens of MB [unverified] | webMAN cache | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers), [webMAN source](https://github.com/aldostools/webMAN-MOD), [IRISMAN](https://github.com/aldostools/IRISMAN) |
| `tmp/wm_res/` | `ps1_bios.bin` (**user-supplied**), `netiso.sprx`, `raw_iso.sprx`, `ps3mon.sprx`, `VshFpsCounter.sprx`, `roms_path.txt`, `roms_extensions.txt` | webMAN, user | Small | CFW/HEN tools (**not cache**) | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers), [webMAN source](https://github.com/aldostools/webMAN-MOD) |
| `tmp/wm_lang`, `wm_icons`, `wm_combo`, `wm_ingame`, `wm_config.bin`, `wm_vsh_menu.cfg`, `art*.txt`, `art.log` | webMAN config, Artemis cheats | webMAN | Small | CFW/HEN tools | [webMAN uninstall](https://github.com/aldostools/webMAN-MOD/wiki/~-Uninstalling-webMAN-MOD) |
| `tmp/gameboot`, `coldboot`, `lines`, `impose`, `psn_icons`, `wallpaper`, `theme`, `earth`, `canyon` | XMB customisation | webMAN | MB | Themes | [webMAN uninstall](https://github.com/aldostools/webMAN-MOD/wiki/~-Uninstalling-webMAN-MOD) |
| `tmp/extract`, `tmp/archive.{rar,7z,tar,…}` | Temporary extraction | webMAN | Up to game size | Packages/tmp (reclaimable) | [webMAN source](https://github.com/aldostools/webMAN-MOD) |
| `tmp/apollo/`, `tmp/apollo.log` | Apollo temp and log | Apollo | Small | Packages/tmp | [apollo-ps3](https://github.com/bucanero/apollo-ps3) |
| `tmp/installer.active`, `tmp/whitelist.cfg`, `blacklist.cfg`, `tmp/lsd/`, `tmp/sbi/` | HEN installer flag and config | PS3HEN | Small | CFW/HEN tools | [PS3HEN](https://github.com/PS3Xploit/PS3HEN) |
| `tmp/game_repo/main/`, `tmp/mmtmp` | multiMAN covers and cache | multiMAN | [unverified] | webMAN/multiMAN cache | Referenced in [webMAN source](https://github.com/aldostools/webMAN-MOD) |
| Root: `screenshot_*.bmp`, `wmm.log`, `prepiso2.log`, `cpursx.html`, `secureid.log`, `ros0_overlay.bin`, `ros1_overlay.bin` | Uncompressed XMB screenshots and webMAN debug output | webMAN | About 6 MB per 1080p BMP (arithmetic) | Media (screenshots); Packages/tmp (logs) | [webMAN source](https://github.com/aldostools/webMAN-MOD) |
| Root, transient: `HENplugin.sprx`, `Latest_HEN_Installer*_signed.pkg`, `theme/PS3HEN.p3t` | HEN stage files, deleted by HEN itself | PS3HEN | MB | Packages | [PS3HEN](https://github.com/PS3Xploit/PS3HEN) |

### Tool apps inside game/

Tool apps live in `game/<ID>` like any other title. Their caches end up inside the "games" category unless HDD Atlas recognises them. Several use IDs that look like disc titles, notably multiMAN's `BLES80608`, so the title-ID prefix alone will misclassify them.

| Title ID | App | Notable contents | Source |
|---|---|---|---|
| `PS3XPLOIT` | PS3HEN (hidden from XMB) | `USRDIR/hen.installed`, `rap.bin` | [PS3HEN](https://github.com/PS3Xploit/PS3HEN) |
| `BLES80608` | multiMAN | `USRDIR/covers/`, `covers_retro/psx/`, `sys/TEMP/`, `sys/SHOWTIME.SELF` | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers), [IRISMAN](https://github.com/aldostools/IRISMAN) |
| `IRISMAN00` | IRISMAN | `USRDIR/TEMP`, **`cache/<TITLEID>/` joined big files** | [IRISMAN](https://github.com/aldostools/IRISMAN) |
| `NPEA00374`, `IMANAGER4` | IRISMAN / Iris Manager covers | `covers/` | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers), [ConsoleMods IrisMAN](https://consolemods.org/wiki/PS3:IrisMAN) |
| `NP0APOLLO` | Apollo Save Tool | `USRDIR/DATA/`, **`USRDIR/CACHE/`** | [apollo-ps3](https://github.com/bucanero/apollo-ps3) |
| `MANAGUNZ0` | ManaGunZ | `USRDIR/covers` | [webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers) |
| `BLES80616`, `RELOADXMB`, `PKGLAUNCH`, `PS2CONFIG`, `PS2U10000`, `PSPM66820`, `PSPC66820`, `XMBMANPLS` | prepISO, XMB reload, ROM/PKG launcher, PS2 config, PS2/PSP launchers, XMB Manager Plus | Small | [webMAN uninstall](https://github.com/aldostools/webMAN-MOD/wiki/~-Uninstalling-webMAN-MOD) |
| `RETROARCH`, `SSNE10000` | RetroArch | `USRDIR/cores` | [webMAN source](https://github.com/aldostools/webMAN-MOD), [IRISMAN](https://github.com/aldostools/IRISMAN) |
| `HTSS00003` | Showtime/Movian | — | [IRISMAN](https://github.com/aldostools/IRISMAN) |
| `HDAT00001` | HDD Atlas itself | Scan cache, log, `deletions.log`, possibly the recycle bin | README |

## Ten signatures identify nearly everything worth parsing

### PARAM.SFO is little-endian on a big-endian console

PARAM.SFO is the most useful file on the drive, and its one trap is endianness. **It is little-endian even though the PS3 is big-endian.** The 0x14-byte header is u32 magic `00 50 53 46` ("\0PSF") at 0x00, version `0x0101` at 0x04, key-table offset at 0x08, data-table offset at 0x0C and entry count at 0x10 ([RPCS3 PSF.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Loader/PSF.cpp); [playstationdev.wiki PSF](https://playstationdev.wiki/pspdevwiki/index.php/Param.sfo)). The index table starts at 0x14. Each 16-byte entry holds u16 key offset, u16 format, u32 length, u32 maximum length and u32 data offset. RPCS3 rejects files where `off_key_table < 0x14`, where the offsets are out of order, or where any key or data offset falls outside its table, and HDD Atlas's `sfo.c` should apply the same checks ([RPCS3 PSF.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Loader/PSF.cpp)).

There are three value formats: `0x0204`, a NUL-terminated UTF-8 string; `0x0404`, a u32; and `0x0004`, **a UTF-8 string that is *not* NUL-terminated, used in system-generated SFOs such as savedata and trophies** ([RPCS3 PSF.h](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Loader/PSF.h); [playstationdev.wiki PSF](https://playstationdev.wiki/pspdevwiki/index.php/Param.sfo)). A parser that relies on NUL termination will read past the end of save fields, so copy exactly `len` bytes for `0x0004`. The authoritative key list (TITLE_ID, APP_VER, CONTENT_ID, SAVEDATA_DIRECTORY, SUB_TITLE, DETAIL, TITLE_xx) could not be fetched, so key names and maximum lengths beyond TITLE, TITLE_ID and CATEGORY are [unverified].

RPCS3's game list defines these CATEGORY codes ([RPCS3 category.h](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/rpcs3qt/category.h)). psdevwiki groups HG, 1P, 2P and MN as HDD content in the Game column, DG as main disc content, and GD and 2D as Game Data Utility content, with **GD covering both add-ons and patches** ([psdevwiki Content Information Files](https://www.psdevwiki.com/ps3/Content_Information_Files)).

| Code | Meaning (RPCS3 constant) | Suggested Atlas category |
|---|---|---|
| HG | HDD game (PSN or homebrew) | Games; sub-label "homebrew" for known tool IDs or non-standard IDs |
| DG | Disc game | Disc backups (appears inside GAMES/ISO PARAM.SFO) |
| GD | PS3 game data: patches, add-ons, installed disc data | Games, attached to the disc title; "updates" in free-up |
| SD | PS3 save | Saves |
| 1P / 2P | PS1 / PS2 Classic | Games (classic) |
| 2G / 2D | PS2 installed / PS2 data | Games / game data |
| PP / MN / PE | PSP game / minis / PSP Remaster | Games |
| MS | PSP or minis save | Saves |
| AM / AP / AT / AV / BV / WT | Music / photo / TV / video apps, broadcast video, Web TV | Games (apps) |
| HM / CB / AS / SF | PlayStation Home, network, store, store front-end | Games (apps) |
| anything else | — | Other (log it; [unverified] codes such as AR, IP and HN exist) |

### Magic bytes and offsets

| Format | Where found | Signature | Fields worth reading | Source |
|---|---|---|---|---|
| PARAM.SFO | Every title, save and trophy folder | `00 50 53 46` @0, ver `0x0101` @4 (LE) | TITLE, TITLE_ID, CATEGORY, APP_VER | [RPCS3 PSF.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Loader/PSF.cpp) |
| SELF / SPRX / EBOOT.BIN | `USRDIR`, `plugins/` | `53 43 45 00` ("SCE\0") @0, BE u32 version 2 @4, u16 attribute @8, **u16 category @0x0A (1 SELF/SPRX, 2 SRVK, 3 SPKG, 4 SSPP, 5 SDIFF, 6 SPSFO)**, u64 data offset @0x10, u64 data size @0x18 | Category, size | [psdevwiki Certified File](https://www.psdevwiki.com/ps3/Certified_File) |
| fSELF | Homebrew EBOOTs | As SELF, attribute `0x8000` | Fake-signed flag | [psdevwiki SELF](https://www.psdevwiki.com/ps3/SELF_-_SPRX) (the Certified File page does not mention 0x8000) |
| ELF / PRX | Homebrew, decrypted | `7F 45 4C 46`, 64-bit BE | — | [psdevwiki SELF](https://www.psdevwiki.com/ps3/SELF_-_SPRX) |
| NPD block | NPDRM SELF supplemental header type 3, start of EDAT/SDAT | `4E 50 44 00` ("NPD\0"), u32 version 1–4 @4, DRM type @8, app type @0x0C, **content ID @0x10 (0x30 bytes)** | Content ID → title | [psdevwiki NPD](https://psdevwiki.com/ps3/NPD) |
| EDAT / SDAT | DLC, PS1 Classics, unlock keys | NPD @0, u32 block size @0x84, **u64 decoded size @0x88**, body @0x100, **plain "EDATA"/"SDATA" + version in the last 16 bytes** | Content ID, real data size | [psdevwiki EDAT](https://psdevwiki.com/ps3/EDAT_files) |
| PKG | `packages/`, root, `vsh/game_pkg`, USB | `7F 50 4B 47` @0, u16 @4 (`80 00` retail, `00 00` debug), u16 type @6 (1 PS3, 2 PSP/Vita), **u64 total size @0x18, content ID @0x30**; metadata ID 0x2 = content type (0x4 GameData, 0x5 GameExec, 0x6 PS1emu, 0x7 PSP, 0x9 Theme, 0xB License, 0xD Avatar, 0xF minis, 0x12 PS2 Classic?, 0x14 PSP Remaster) | Already-installed check, label | [psdevwiki PKG files](https://psdevwiki.com/ps3/PKG_files) |
| TROPHY.TRP | `PS3_GAME/TROPDIR/<NPCOMMID>/` in backups and ISOs | BE 64-byte header (u32 magic, u32 version, u64 file size @8, u32 file count @0x10, u32 entry size @0x14, dev flag, SHA-1); 64-byte entries (name[32], u64 offset, u64 size) | Trophy name and icon without installing | [RPCS3 TRP.h](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Loader/TRP.h), [Red Squirrel](https://redsquirrel87.altervista.org/doku.php/trophy.trp-extractor) |
| TROPCONF.SFM / TROP*.SFM | Installed trophy folders | Text "Sce-Np-Trophy-Signature" @0x40 | Game title for trophy folders | [psdevwiki Trophy files](https://psdevwiki.com/ps3/Trophy_files) |
| PARAM.PFD | Save, trophy and `_TROPSYS_` folders | 8-byte magic field `0x50464442` ("PFDB") @0, version 3 (or 4) @8 | Marks a protected folder | [psdevwiki PARAM.PFD](https://psdevwiki.com/ps3/PARAM.PFD) |
| RIF / act.dat | `home/<u>/exdata` | No magic sourced; RIF signature checked @0x70, act.dat signature @0x1010 | Licence presence | [psdevwiki NPDRM](https://psdevwiki.com/ps3/NPDRM) |
| RAP | `home/<u>/exdata` (RPCS3), `/dev_hdd0/exdata` (HEN) | Named `<CONTENT_ID>.rap`; no magic | Content ID from filename | [RPCS3 system_utils.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/system_utils.cpp), [PS3HEN](https://github.com/PS3Xploit/PS3HEN) |
| PNG (ICON0, PIC0-2) | Every title | `89 50 4E 47`; ICON0 320×176 (only mandatory file), PIC1 1920×1080 | Icon | [psdevwiki Content Information Files](https://www.psdevwiki.com/ps3/Content_Information_Files) |
| Disc layout | GAMES backups, ISOs | `PS3_DISC.SFB` present → root is `PS3_GAME/` | Backup vs install | [RPCS3 system_utils.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/system_utils.cpp) |

These signatures were **not** sourced and need checking against real files: TRP magic `DC A2 4D 00`, PS3 ISO "CD001" at 0x8001 with "PlayStation3" at 0x800, PBP `00 50 42 50`, P3T "P3TF", RCO `00 50 52 46`, QRC "QRCF", PS3_DISC.SFB ".SFB", PUP "SCEUF", PSARC "PSAR", PAM "PAMF", LIC.DAT "PS3LICDA", and the usual sizes of RIF (0x98), RAP (16 bytes) and act.dat (0x1038).

### Content IDs tie licences, PKGs and DLC to titles

**A content ID is up to 0x30 characters of `[A-Za-z0-9_-]`** ([RPCS3 system_utils.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/system_utils.cpp)) in the form `UP0001-NPUB30001_00-…`, so the 9-character title ID sits at offset 7. That gives a free join across four places: a RAP's filename (`<CONTENT_ID>.rap`), a PKG header at 0x30, an EDAT/SDAT's NPD block at 0x10, and an NPDRM EBOOT's supplemental header, which can be read without decrypting anything. RIFs work differently. For Network and Local DRM a RIF is tied to `act.dat`, and **Free DRM content is not bound to act.dat** ([psdevwiki RIF](https://psdevwiki.com/ps3/RIF); [psdevwiki NPDRM](https://psdevwiki.com/ps3/NPDRM)). An `NP**` HG game with no licence is therefore not necessarily broken. Licences are tiny, so they matter for labels and not for space.

## Where the README's rules will misreport space

### "Unaccounted" absorbs a known 8% and an unknown sign

As written, the README's "the system's own partitions" and "unaccounted space" categories have no way to tell the reserve, the UFS2 metadata and fragment rounding apart. The first fix concerns syscall 840. psdevwiki names its outputs `total_free` and `avail_free` ([psdevwiki LV2 syscalls](https://www.psdevwiki.com/ps3/LV2_Functions_and_Syscalls)). **webMAN treats the first output as device capacity** and shows `100*(devSize-freeSize)/devSize` as percent used ([webMAN source, include/file/file_devs.h](https://github.com/aldostools/webMAN-MOD)). The two readings disagree. If psdevwiki's naming is right, the gap between the two outputs is exactly the minimum-free reserve, and that becomes HDD Atlas's "OS reserve" slice. On a nearly empty drive both readings give similar numbers, so the test has to be run on a well-filled one.

`fsx_ps3.c` should log both raw outputs, alongside `sysFsGetFreeSize` (block size × free blocks, as IRISMAN does ([IRISMAN source](https://github.com/aldostools/IRISMAN))) and `hddFreeSizeKB` from `sysutil/game.h` ([PSL1GHT game.h](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/include/sysutil/game.h)). Comparing all four against the XMB's System Information settles the question once per console.

The second fix is to make the reserve its own slice, labelled "Reserved by the system (8%; 1% if webMAN unlockhdd is on)". Without it, **running `/unlockhdd.ps3?enable` makes about 7% of the drive appear as new free space between two scans**, which "space over time" and "changes since the last scan" will report as an unexplained drop in used space. Store the detected reserve percentage with each scan in `history.c` so the jump can be explained. webMAN reads the current minimum-free percentage when it reports "min: %u%% / optim: %u" ([webMAN source, include/cmd/unlockhdd.h](https://github.com/aldostools/webMAN-MOD)), and copying that read path, never the write, would show the setting directly.

Third, compute "the system's own partitions" from the waterfall (raw IDENTIFY capacity minus the dev_hdd0 total) rather than from a constant. Say "2 GiB game cache + 256 MiB system flash" on NOR consoles and "2 GiB game cache" on NAND ones, and count `/dev_flash*` as part of VFLASH, never on top of it.

Fourth, **estimate on-disk size by rounding each file up to the 2 KiB fragment, not to `st_blksize`**. UFS2 stores file tails in fragments, so rounding to the 16 KiB block observed on a 320 GB Slim ([Woolfy025 gist](https://gist.github.com/Woolfy025/6b275e06b166fffd50e059f4198e1aa8)) overstates small-file folders such as trophies, saves and webMAN covers. Whether `st_blksize` on dev_hdd0 reports 16384 is [unverified]; syscall 822 `sys_fs_get_block_size` returns the block size, and the meaning of its extra output is unknown ([psdevwiki LV2 syscalls](https://www.psdevwiki.com/ps3/LV2_Functions_and_Syscalls)).

Finally, report unaccounted space as a signed number. **A negative unaccounted value is the clearest available evidence of hard-link double counting.**

### Seven labelling traps

`/dev_hdd0/video` is in webMAN's PS3ISO scan list as a "stealth" folder ([webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers)). Classifying by folder will put multi-GB game ISOs under Media, outside the delete allow-list and outside "games, all together". Check for a PS3 ISO signature before classifying anything there as Media.

`/dev_hdd0/exdata` at the root is HEN's RAP folder, created at every boot ([PS3HEN](https://github.com/PS3Xploit/PS3HEN)). If the licences category only matches `home/*/exdata`, these RAPs will drop into "system folders" or "unaccounted".

The README's "updates and saves for games that aren't here" will mislabel a lot of GD data. A disc title's patch folder is not orphaned just because no backup exists. The disc may be physical. The ISO may be on an NTFS/exFAT drive, which webMAN tracks through `.ntfs[...]` stubs in `tmp/wmtmp`. It may be on a ps3netsrv share (`net*`). It may be on a USB drive HDD Atlas has not scanned yet. Treat B-prefix GD folders as "disc game data (game not found on scanned drives)" and lower their priority in free-up. As extra evidence, read the stub names in wmtmp and webMAN's `xmlhost/game_plugin/mygames.xml`, whose format is [unverified].

Tool apps inflate "games". IRISMAN's cache, multiMAN's covers and Apollo's CACHE all live under `game/<ID>`, and `BLES80608` looks like a disc title. Key them off a known-tool table (the one above) plus CATEGORY HG, never off the prefix. `PS3XPLOIT` should be labelled "PS3HEN (hidden)" and never offered for deletion.

`/dev_hdd0/tmp` is not all temp. It holds HEN's whitelist, blacklist, `lsd` and `sbi` config, webMAN's `wm_res`/`wm_lang`/config files, and the XMB customisation folders. Split it into webMAN cache (`wmtmp`), reclaimable temp (`extract`, `archive.*`, `apollo`) and CFW/HEN tools (the rest), rather than reporting one "packages/tmp" number.

HG trials that have been unlocked keep their full-game content in `game/<ID>/C00/` with its own PARAM.SFO ([RPCS3 system_utils.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/system_utils.cpp)). Read titles from there when it exists.

HDD Atlas's own `HDAT00001` folder (scan cache, logs and, depending on `bin.c`, the recycle bin) should be shown as its own row. Otherwise items waiting in the bin count as "games" for 7 days.

### Hard links: webMAN shares bytes that PSL1GHT cannot see

webMAN's README lists "relink identical files in /dev_hdd0/GAMES and /dev_hdd0/game" as a feature. The implementation (`/stat.ps3<path>&id=<title_id>`) compares size and, unless `?fast` is given, MD5. **On a match it unlinks `/dev_hdd0/game/<ID>/USRDIR/<file>` and calls `sysLv2FsLink(source, game_path)`, logging "lnk->" to `filelist.txt`** ([webMAN MOD README and source, include/file/file_size.h, include/cmd/stat_chmod.h](https://github.com/aldostools/webMAN-MOD)). The disc backup keeps the original and the install-data folder gets a second name for the same inode.

PSL1GHT cannot see this. `sysFSStat` has no `st_ino`, `st_nlink` or `st_blocks`, and newlib's `convertLv2Stat` zeroes those fields ([PSL1GHT lv2/sysfs.h](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/include/lv2/sysfs.h); [PSL1GHT librt/fstat.c](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/librt/fstat.c)). So HDD Atlas counts every relinked file twice. Three features overstate as a result:

- "games, all together" adds both copies to the title's total.
- "identical files" (byte for byte) will correctly call the pair identical but implies deleting one frees space, **when deleting either name frees zero bytes**.
- "extra copies of games" overstates what deleting the GAMES backup recovers, since the linked files survive in `game/<ID>`.

No direct test exists. Four indirect signals can be combined, and all except the last are untested inference:

1. **Path pairing.** A file with the same relative path and size under `GAMES/<x>/PS3_GAME/USRDIR/` and `game/<TITLEID>/USRDIR/`, where `<x>`'s PARAM.SFO has that TITLE_ID.
2. **Matching times.** Hard links share one inode, so `st_mtime` and `st_ctime` must match exactly, while an installer's copy will usually have its own timestamps.
3. **webMAN's log.** "lnk->" lines in `/dev_hdd0/tmp/wmtmp/filelist.txt`, if it has not since been overwritten by a file search.
4. **The global check.** If the sum of file sizes exceeds `used` from syscall 840, something is counted twice.

Mark matched pairs "possibly shared (webMAN link)", count them once in drive totals, and show reclaimable space as zero for them in the identical-files and extra-copies views.

### Licences can be joined to titles at almost no cost

Licence files are small, but they make good labels. Parse the content ID from RAP filenames in both `home/<u>/exdata` and the root `exdata`, from NPD headers of EDATs in `USRDIR`, and from PKG headers. Then attach them to the title rows in "games, all together". Two useful flags fall out: "licence present, game not installed", which is cheap to keep and explains why a re-download works, and "PSN game with no RAP/RIF found", which is informational only because Free DRM content needs no act.dat binding ([psdevwiki NPDRM](https://psdevwiki.com/ps3/NPDRM)). The same join gives leftover PKGs their strongest reason line, "this package's content ID is already installed as `game/<TITLEID>`". Read the 0x18 total size and the content type to say whether it is a game, a patch or a theme ([psdevwiki PKG files](https://psdevwiki.com/ps3/PKG_files)).

## Free-up candidates and the delete allow-list need sharper edges

### Candidates the README does not yet list

| Candidate | Path | Evidence | Size | Safety note | Suggested handling |
|---|---|---|---|---|---|
| IRISMAN joined-file cache | `IRISMAN00/…/cache/<TITLEID>/` (exact base path [unverified]) | `LoadCacheDatas()` lists each entry as "%s (%1.2f GB)" with "Delete %s Cache?"; it joins `.66600` split sets from USB games onto the internal HDD ([IRISMAN source](https://github.com/aldostools/IRISMAN), [ConsoleMods](https://consolemods.org/wiki/PS3:IrisMAN)) | GB per title | That USB game will not run until IRISMAN rebuilds the cache from the USB copy | Add, one row per title; name the title from `name_entry` |
| Apollo self-update PKG | `/dev_hdd0/packages/apollo-ps3.pkg` | Downloaded by `settings.c` and never deleted ([apollo-ps3](https://github.com/bucanero/apollo-ps3)) | MB | Safe once installed (compare with `NP0APOLLO` APP_VER) | Already covered if "leftover packages" includes `packages/` |
| Apollo cache | `NP0APOLLO/USRDIR/CACHE/`, `/dev_hdd0/tmp/apollo/` | Online-save zips, web pages, list files ([apollo-ps3](https://github.com/bucanero/apollo-ps3)) | Small to tens of MB [unverified] | Re-downloaded on demand | Add as cache |
| webMAN `~tmp.iso` | `/dev_hdd0/{GAMES…BDISO}/~tmp.iso` | Created for mounts, skipped by webMAN's scanner, unlinked by `do_umount` ([webMAN source](https://github.com/aldostools/webMAN-MOD)) | [unverified] | Only a leftover if nothing is mounted | Add with a "unmount in webMAN first" note and an age guard |
| webMAN extract folder | `/dev_hdd0/tmp/extract` | Deleted recursively on unmount when `deliso` is set ([webMAN source](https://github.com/aldostools/webMAN-MOD)) | Up to game size | Not while a game is mounted | Add |
| Temp archives | `/dev_hdd0/tmp/archive.{rar,7z,tar,tgz,tar.gz,tar.bz2}` | webMAN source ([webMAN](https://github.com/aldostools/webMAN-MOD)) | Varies | Leftover after extraction | Add |
| `PS3_UPDATE` inside folder backups | `GAMES/<x>/PS3_UPDATE/` | ConsoleMods recommends deleting it; webMAN redirects `dev_bdvd/PS3_UPDATE` anyway ([ConsoleMods multiMAN](https://consolemods.org/wiki/PS3:MultiMAN), [webMAN](https://github.com/aldostools/webMAN-MOD)) | A few hundred MB [unverified] | Firmware the console must never install on HEN/CFW; cannot be trimmed from ISOs | Add as a sub-item of the backup; extend the allow-list to this subfolder |
| Pending firmware | `/dev_hdd0/updater/**/PS3UPDAT.PUP` | [unverified] | About 200 MB [unverified] | Delete the file only; HEN recreates `updater/01` | Add, files only |
| PS2 ISO play copies | `/dev_hdd0/PS2ISO/*` with the same name on USB/NTFS/net | webMAN copies PS2ISO from USB, NTFS or net to HDD0 to play ([webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers)) | Up to DVD-9 size [unverified] | Safe if the original still exists elsewhere | Feed into cross-drive duplicates |
| Uncompressed screenshots | `/dev_hdd0/screenshot_*.bmp` | webMAN source ([webMAN](https://github.com/aldostools/webMAN-MOD)) | About 6 MB each (arithmetic) | User content | Show, but don't auto-recommend; ideally offer under Media |
| HEN installer leftovers | `/dev_hdd0/Latest_HEN_Installer*_signed.pkg`, `theme/PS3HEN.p3t`, `HENplugin.sprx` | HEN unlinks these at startup ([PS3HEN](https://github.com/PS3Xploit/PS3HEN)) | MB | Normally already gone when HDD Atlas runs on HEN | Make sure root-level `.pkg` files count as leftover packages |
| XMB download queue | `/dev_hdd0/vsh/game_pkg/` | Path referenced by IRISMAN ([source](https://github.com/aldostools/IRISMAN)); meaning [unverified] | Multi-GB | Could be a pending install | Report only; never on the allow-list |
| multiMAN cover cache | `BLES80608/USRDIR/covers*`, `tmp/game_repo`, `tmp/mmtmp` | Referenced in webMAN ([webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers)) | [unverified] | Regenerable [unverified] | Report as cache |
| Old trophy sets | `home/<u>/trophy/_BU_*` | Created by the rename on upgrade ([Trophy files](https://psdevwiki.com/ps3/Trophy_files)) | Small | PFD-signed trophy data | Report only |
| Logs | `tmp/apollo.log`, `wmm.log`, `prepiso2.log`, `tmp/log.txt`, `art.log` | webMAN/Apollo source | KB | Safe | Optional |

### Reviewing the allow-list item by item

**Disc images.** Extend this to `/dev_hdd0/video/*.iso` once the PS3 ISO check passes, and treat `.iso.0–.63` and `.key/.dkey` as one unit with the image. A split set should never be partly deleted.

**Folder backups in GAMES.** Extend this to `GAMEZ`, `PSXGAMES` and `PS2DISC`, and aggregate `.666xx` parts. Before showing "frees X", subtract files that the hard-link heuristic marks as shared with `game/<ID>`.

**Leftover packages.** Look in `packages/`, the drive root (HEN installers, user drops) and USB `packages`/`PKG`, and give each PKG its content-ID join. Exclude `vsh/game_pkg`.

**ROMs.** Fine as is, since they are user content.

**webMAN's cache.** This needs the tightest definition. webMAN's own `/delete.ps3?wmtmp` wipes `/dev_hdd0/tmp/wmtmp`, and `?wmreset` wipes wmtmp plus the config ([webMAN wiki: Web Commands](https://github.com/aldostools/webMAN-MOD/wiki/Web-Commands)). That supports deleting wmtmp, and nothing beyond it, as cache. Inside wmtmp, though, **"ISOs on exFAT/NTFS need matching `.ntfs[***]` files in `/dev_hdd0/tmp/wmtmp`"**, and only webMAN's refresh (`/refresh.ps3?prepntfs`) or prepISO recreates them ([webMAN wiki](https://github.com/aldostools/webMAN-MOD/wiki/Game-Paths-&-Covers)). Deleting them hides NTFS games until the user refreshes. `slaunch.cfg` and `last_games.bin` reset sLaunch settings and history. IRISMAN's `%s.iso` entries in wmtmp are of unknown size and purpose. Offer two scopes, "covers only" and "everything (NTFS games need a webMAN refresh afterwards)". Hard-exclude `tmp/wm_res` (it holds the user's `ps1_bios.bin` and the `netiso`/`raw_iso` plugins), `wm_lang`, `wm_icons`, `wm_combo`, `wm_ingame` and `wm_config.bin`.

**`game/_INST_*` and `game/_DEL_*`.** No source in this research documented Sony's naming for in-progress installs or deletes. The only source found on install leftovers uses `_INSTALL`, `_TEMP` and `_SETUP` suffixes and reads as AI-generated ([ps2bios.gitlab.io](https://ps2bios.gitlab.io/home/for-ps3iso-games-within--devhdd0-game----are-any-of-these-folders-safe-to-delete-and-not-effect-each-games-content)). Keep the rule, but guard it:

- Refuse `_INST_*` folders modified in the last 24 hours.
- Refuse while `/dev_hdd0/vsh/game_pkg` contains files.
- Refuse while HEN's `/dev_hdd0/tmp/installer.active` exists ([PS3HEN](https://github.com/PS3Xploit/PS3HEN)).
- Tell the user that an install interrupted by a crash must be started again from its PKG.

These guards are inference. Whether the XMB resumes an `_INST_` folder after a reboot is [unverified]. Recycle-bin moves are renames on the same UFS2 volume, so the bin itself creates no risk, but free space will not change until the bin is purged.

### HEN puts back what HDD Atlas deletes

`create_default_dirs()` recreates the 14 HEN folders whenever HEN is enabled ([PS3HEN source](https://github.com/PS3Xploit/PS3HEN)). webMAN's `/rmdir.ps3` deletes empty default folders ([webMAN wiki: Web Commands](https://github.com/aldostools/webMAN-MOD/wiki/Web-Commands)). On a console running both, those folders appear and disappear between scans. Three adjustments follow:

- Keep a known-default list and never show those folders as free-up items or as deletable rows.
- Hide empty default folders from "changes since the last scan" so they don't show up as noise.
- Label them "created by PS3HEN at every boot; harmless when empty".

The same list protects `exdata`, `plugins` and `updater/01` from being swept up in a "remove empty folders" feature, if one is ever added.

## PSL1GHT hides inodes, link counts and block counts

The scanner sits on the PSL1GHT layer, and the research turned up eight specific traps there, all from current source. The scanner is already iterative, which removes the biggest crash risk, since prior art recurses with large stack frames.

| Gotcha | Fact | Consequence for HDD Atlas | Source |
|---|---|---|---|
| No allocation data | `sysFSStat` = mode, uid, gid, atime, mtime, ctime, `st_size`, `st_blksize`; newlib zeroes `st_blocks`, `st_ino`, `st_nlink`, `st_dev` | Estimate on-disk size yourself; hard links can't be detected; `st_dev` can't mark mount boundaries | [sysfs.h](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/include/lv2/sysfs.h), [fstat.c](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/librt/fstat.c) |
| Syscall 840 has no wrapper | Not in `sys/file.h`; webMAN calls `system_call_3(840, dev, &devSize, &freeSize)`; psdevwiki names the outputs `total_free`/`avail_free` | Log both outputs and calibrate against the XMB | [file.h](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/include/sys/file.h), [webMAN](https://github.com/aldostools/webMAN-MOD), [psdevwiki](https://www.psdevwiki.com/ps3/LV2_Functions_and_Syscalls) |
| `sysFsGetDirectoryEntries` stride | SPRX function (NID 0x9b882495); PSL1GHT's `sysFSDirectoryEntry` embeds `d_name[MAXPATHLEN+1]`, and the firmware's native entry is believed to use a 256-byte name [unverified] | Ask for one entry per call (as webMAN does with `sizeof(entry)`), or define and hardware-test a native-layout struct before batching | [sysfs.h](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/include/lv2/sysfs.h), [exports.h](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/sprx/libsysfs/exports.h), [webMAN](https://github.com/aldostools/webMAN-MOD) |
| "." and ".." | webMAN skips them by hand after `cellFsGetDirectoryEntries` | Filter explicitly | [webMAN file_size.h](https://github.com/aldostools/webMAN-MOD) |
| `d_type` values | `DT_UNKNOWN 0`, `DT_DIR 1`, `DT_REG 2`, `DT_LNK 3`, from syscall 806, **not** `CELL_FS_S_IF*` mode bits | Decide whether to recurse without stat; never recurse into `DT_LNK`; use `S_ISDIR` only on `st_mode` | [ps3toolchain newlib patch](https://github.com/ps3dev/ps3toolchain/blob/master/patches/newlib-1.20.0-PS3.patch) |
| Directory API limits | newlib `rewinddir`/`seekdir` return ENOSYS; one syscall per entry; ~1 KB dirent copied per entry | Never rewind; read a directory into a heap list, close it, then descend (keeps open fds constant, and the LV2 fd limit is undocumented) | [dirent.c](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/librt/dirent.c) |
| Stack size | Apollo's worker threads use 16 KiB stacks; webMAN and IRISMAN keep a path buffer plus a ~1 KB dirent per recursion level | Keep the dirent and path buffers off the worker stack, or size the thread stack generously | [apollo-ps3](https://github.com/bucanero/apollo-ps3), [IRISMAN](https://github.com/aldostools/IRISMAN), [thread.h](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/include/sys/thread.h) |
| Mounts and permissions | Syscalls 801–842 are callable on retail (CEX); webMAN discovers drives by iterating `/` and matching `dev_hdd`; `/dev_hdd1` needs a mount (`CELL_FS_UTILITY:HDD1`) | Discover mounts by listing `/`; count paths that fail to open as an "unreadable" bucket (the path probe log is the right place); don't mount dev_hdd1 or dev_blind from a read-only tool | [psdevwiki](https://www.psdevwiki.com/ps3/LV2_Functions_and_Syscalls), [webMAN](https://github.com/aldostools/webMAN-MOD) |

Two smaller notes. The external drives the README scans are FAT only unless libntfs is linked, which IRISMAN and webMAN both use for `ntfs*` roots ([webMAN source](https://github.com/aldostools/webMAN-MOD)). And no benchmark of PS3 traversal speed exists anywhere in the sources, so HDD Atlas's own scan timings would be new data worth publishing.

## Verify these on the HEN and CFW consoles

**What could and could not be sourced.** The partition layout, the PSL1GHT API facts, the file-format offsets that come from psdevwiki pages that loaded, and every webMAN, PS3HEN, IRISMAN and Apollo path rest on primary sources: wiki pages that loaded, or current source code read directly. The rest of Sony's own layer of `/dev_hdd0` does not, and neither do the PARAM.SFO key list, most typical sizes, the NAND/NOR model mapping, Sony's `_INST_`/`_DEL_` naming and several magic numbers. Those came from background knowledge, because these sources were blocked or failed: psdevwiki's PARAM.SFO, Dev_hdd0, Partitions, Mount_Paths, VFS, Theme files, PSARC and PS3_DISC.SFB pages, all of psx-place, nextgenupdate, and multiMAN's closed-source internals. Run the checks below with HDD Atlas's path probe log, an FTP listing from webMAN, or a hex dump before hard-coding anything.

- [ ] Log both outputs of syscall 840, `sysFsGetFreeSize` and `hddFreeSizeKB` on a drive that is at least half full; compare them with XMB System Information and record which output is capacity.
- [ ] On the CFW console, run `/unlockhdd.ps3?enable`, rescan, then `?disable`, and confirm the free-space jump equals about 7% of capacity.
- [ ] Read the ATA IDENTIFY capacity, subtract the dev_hdd0 total, and confirm the remainder is about 2.25 GiB on NOR (CECHK, Slim) and about 2 GiB on NAND.
- [ ] Record `st_blksize` for a dev_hdd0 file and the output of syscall 822; confirm the 16 KiB block / 2 KiB fragment geometry.
- [ ] List the top level of `/dev_hdd0` on both consoles and check off `drm`, `crash_report`, `data`, `disc`, `hdd`, `etc`, `kernel`, `mms`, `vsh`, `vm`, `updater`, `theme`, `music`, `photo`, `video` against the map above.
- [ ] List `home/00000001/` and confirm the subfolder names (`savedata`, `trophy`, `exdata`, `friendim`, `minis_savedata`, `ps2emu2_savedata`, `webbrowser`, `np`, `localusername`).
- [ ] Start a PKG install, cut the power or quit partway, and record the exact name of the folder left in `game/` (`_INST_…`?); delete a game from the XMB and check for `_DEL_…`; reboot and see whether the XMB cleans up or resumes either one.
- [ ] Check `vsh/` contents with and without a pending PSN download, to confirm what `game_pkg/` and `task.dat` hold.
- [ ] Use webMAN's `/stat.ps3/dev_hdd0/GAMES/<x>&id=<TITLEID>` relink on a test title, then confirm that HDD Atlas's sum exceeds syscall-840 used space and that the linked pairs share `st_mtime`/`st_ctime`; look for "lnk->" in `tmp/wmtmp/filelist.txt`.
- [ ] Delete an empty `ROMS` folder, reboot with HEN, and confirm it comes back; confirm the CFW console (no HEN) does not recreate it.
- [ ] Hex-dump one each of TROPHY.TRP (magic `DC A2 4D 00`?), a PS3 ISO (CD001 at 0x8001, "PlayStation3" at 0x800?), PS3UPDAT.PUP ("SCEUF"?), a `.p3t` ("P3TF"?), a RAP (16 bytes?), a RIF (0x98?) and `act.dat` (0x1038?).
- [ ] Parse a save's PARAM.SFO and confirm that format `0x0004` strings are handled without NUL termination; dump its keys to confirm `SAVEDATA_DIRECTORY`, `SUB_TITLE`, `DETAIL`.
- [ ] Check IRISMAN's base path for `cache/<TITLEID>` (USRDIR or the app root) and the size of one cached title.
- [ ] Run `sysFsGetDirectoryEntries` with a two-entry buffer on a known folder and check whether the second entry's name lands at PSL1GHT's stride.
- [ ] Confirm whether `/dev_hdd1` shows up when listing `/` from a PSL1GHT app on HEN and on CFW without mounting it.
- [ ] Find where HDD Atlas's recycle bin lives on dev_hdd0 and USB, and make sure the scanner gives it its own row.
- [ ] Check a `GAMES/<x>/PS3_UPDATE/PS3UPDAT.PUP` size and a `/dev_hdd0/updater` PUP size to replace the "few hundred MB / ~200 MB" estimates.

## Conclusion

HDD Atlas's remaining accuracy problems come from the gap between what the filesystem API reports and what the drive actually stores, not from missing folders. The partition slices, the 8% reserve and fragment rounding are all deterministic, so the "unaccounted" figure can shrink to a small, signed remainder that becomes a diagnostic in its own right: a negative remainder flags webMAN's hard links, and a jump of about 7% flags unlockhdd. That turns a vague leftover bucket into evidence the app can explain.

The second gain comes from the content ID and the tool-ID table. With them, HDD Atlas can say where a byte came from as well as how big it is: this PKG is already installed, this cache belongs to IRISMAN, this folder is HEN's and will come back, this patch belongs to a disc that may be in the drive tray. No existing PS3 tool combines a size scan with that kind of provenance. webMAN's `/stat.ps3` gives one recursive total per request ([webMAN wiki: Web Commands](https://github.com/aldostools/webMAN-MOD/wiki/Web-Commands)), and IRISMAN only sizes its own cache. That provenance, rather than raw scanning, is the part of HDD Atlas worth investing in next.
