# HDD Atlas 1.0.1

Install `HDDAtlas-v1.0.1.pkg` over 1.0.0 (Game > Install Package Files, or webMAN). Your scans, settings, recycle bin and health readings are kept.

## New

- **Stuck scans give up.** If a drive stops answering, the scan gives up after 5 minutes and shows where it got stuck. Please send the log (`hddatlas_log.txt`, in `/dev_hdd0/game/HDAT00001/USRDIR`) to **ps3.world/contact**. Restart the console before scanning again.
- **SSD support in Drive health.** An internal SSD shows how much life it has left, with SSD names and temperature limits.
- **Warnings for a failing drive.** If a scan hits read errors on the internal drive, the app tells you and points you to Drive health.
- **What to do if your drive is wearing out.** Drive health has a short guide: back up your saves, licences and trophies first, delete what you don't need, avoid rescanning, and replace the drive. Swap guide: **ps3.world/upgrade-storage**.

## Fixed

- Circle stops a scan straight away, even in a big folder.
- A damaged folder that never ends no longer makes the scan run forever.
- A stuck drive no longer freezes the app afterwards.
- NTFS drives no longer freeze the app when one is busy or stops answering.
- A USB stick that stops answering no longer freezes the app.
- The progress bar no longer sits at 99%.
- PS1 disc images (`.bin`) are read much faster.
- Drive health's table headers no longer overlap in some languages.
- Quitting no longer hangs when an NTFS drive is stuck.

## Source

`HDDAtlas-v1.0.1-source.zip`: build with `sh tools/build.sh` (see README.md).
