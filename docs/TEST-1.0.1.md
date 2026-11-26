# HDD Atlas 1.0.1: hardware test

Install `out/HDDAtlas-v1.0.1.pkg` over 1.0.0 (RELEASE-NOTES-1.0.1.md says what's in it). Most of the changes are about failing drives, which are hard to stage, so this list checks that nothing that worked before broke, plus the parts you can try. (1.0.2 and 1.0.3 were never released: they're folded in here.)

## 1. Scans as before

- [ ] HDD: a full scan finishes, with the same totals as 1.0.0's last scan (Last scan, then the overview).
- [ ] The bar and the strip at the bottom right show a percentage that moves. If the strip shows "Scanning HDD · N files" with no percentage, the files add up to more than the used space. Note it, with the drive's used space and the scan's total.
- [ ] The log has `scan /dev_hdd0: started` and no `listed 200000 entries` line.
- [ ] A FAT32 USB stick and an NTFS drive: each scans and finishes.

## 2. Stop in the middle of a big folder

- [ ] Rescan the HDD. While the path on screen is in `game/` (193,000 files on one user's drive; yours may be smaller), press Circle. "Scan stopped" comes within a second, not when the folder's done.

## 3. PS1 images (if you have raw `.bin` PS1 games in PSXISO)

- [ ] Rescan Disc game backups (Square on it): their IDs (SCUS..., SLES...) and the 1P code show as before.
- [ ] On a USB or NTFS drive with many of them, the scan is noticeably quicker than 1.0.0's (the log's `finished in` line).

## 4. Free space and plugging drives in

- [ ] Plug in a USB stick while the overview's showing: it appears within about 5 seconds (L1/R1), as before.
- [ ] Pull it out: it goes within about 5 seconds.
- [ ] Delete something (with deleting on): the free space at the top changes within a few seconds.
- [ ] Plug in an NTFS drive **while another NTFS drive is being scanned**. The log says `waiting for the NTFS drive being read to be done, to mount it`, and the new drive is mounted a few seconds after that scan finishes.

## 5. NTFS: the screen doesn't wait

- [ ] During an NTFS drive's scan, switch to another drive and open its START menu and its recycle bin (Tools). They open at once, as before.
- [ ] If the log ever shows `ntfs: a drive call held the lock over 1 s: the screen's call fails (busy)`, note what you'd just pressed. Nothing should freeze.

## 6. A stuck drive (only if you have a failing one)

- [ ] After the stuck panel (5 minutes), press X. Then START, X on a category, and Triangle on that drive: each shows "… stopped answering during its scan: restart the console to use it again" and nothing freezes.
- [ ] L1/R1 to another drive: its menu, lists and deletes work. Triangle there says no scan can start till the console restarts.
- [ ] Quitting works (HOME_QUIT, or the XMB).
- [ ] **A stuck NTFS drive blocks all NTFS drives.** Only if you have a failing NTFS drive: after its stuck panel, L1/R1 to another NTFS drive. START, X and Square there say "… stopped answering during its scan, and every NTFS drive waits on it: restart the console to use them again". The HDD and FAT32 sticks still work.

## 7. Healthy drive: nothing new shows

- [ ] A full HDD scan ends with no panel, and the log has no `read errors: Drive health suggested` and no `nothing new for` line.
- [ ] Starting a scan shows the usual "Scanning: things may be slow…" note, not "Drive health found this drive wearing out".
- [ ] Drive health reads as in 1.0.0, its history keeps 1.0.0's readings, and there's no "What to do" (X) hint. With an internal SSD: it's shown as one, with its life left.
- [ ] In German at 4:3, Drive health's table headers (Tiefstwert, Schwelle) don't overlap.

## 8. NTFS busy calls

- [ ] If the log shows `the screen's call fails (busy)`, the later lines within 5 seconds say `over 0.05 s`, not `over 1.00 s`. Nothing freezes.
