/*
report.h: a scan written out to read on a PC: a text report (what's there,
by category, every item, the largest files, what changed) and a CSV of the
items for a spreadsheet. Written to dir (made if need be), named for the
drive and the time: HDDAtlas_dev_hdd0_20261109-1700.txt / .csv.
*/
#ifndef REPORT_H
#define REPORT_H

#include "scan.h"

// 0 when both were written; out gets the text report's path
int report_write(const scan_t *s, const scan_changes_t *changes, const char *drive_name, const char *dir, char *out, int size);

#endif
