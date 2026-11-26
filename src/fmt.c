/*
fmt.c: sizes and counts as the screens (and the report) write them. From PS3 World's
ui.c (Fmt_Size, Fmt_Count), on their own so the report needn't bring the drawing in.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lang.h"
#include "ui_kit.h"

int kit_units_1000;		// (HDD Atlas: Settings > Sizes; 1024 as the XMB counts, or 1000 as drive makers do)

// the units in the language on screen (French Go, Russian ГБ...) or, for the report, English
void kit_fmt_size_in(long long b, char *out, int size, int local)
{
	double k = kit_units_1000 ? 1000.0 : 1024.0;
	const char *u[5] = {N_("TB"), N_("GB"), N_("MB"), N_("KB"), N_("B")};
	if (local) for (int i = 0; i < 5; i++) u[i] = tr(u[i]);
	if (b >= k * k * k * 1000) snprintf(out, size, "%.2f %s", b / (k * k * k * k), u[0]);
	else if (b >= k * k * k) snprintf(out, size, "%.2f %s", b / (k * k * k), u[1]);
	else if (b >= k * k) snprintf(out, size, "%.1f %s", b / (k * k), u[2]);
	else if (b >= k) snprintf(out, size, "%lld %s", (long long)(b / k), u[3]);
	else snprintf(out, size, "%lld %s", b, u[4]);
}

void kit_fmt_size(long long b, char *out, int size) { kit_fmt_size_in(b, out, size, 1); }

void kit_fmt_count(long long n, char *out, int size)
{
	// (PS3 World's went to 999,999; a drive's files can run past a million)
	if (n >= 1000000) snprintf(out, size, "%lld,%03lld,%03lld", n / 1000000, n / 1000 % 1000, n % 1000);
	else if (n >= 1000) snprintf(out, size, "%lld,%03lld", n / 1000, n % 1000);
	else snprintf(out, size, "%lld", n);
}

/*
==================
Names that aren't UTF-8: the PS3 keeps a file's name as the bytes it was given, and a theme
or a copy from Windows can give it Windows-1252 ("PlayStation\xaePlus 2nd Anniversary.p3t").
A byte that doesn't start a whole UTF-8 character is read as Windows-1252's.
==================
*/
uint32_t kit_cp1252(unsigned char c)
{
	static const uint16_t hi[32] = {		// 0x80..0x9f (the rest are Latin-1's own)
		0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
		0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178};
	return c >= 0x80 && c < 0xa0 ? hi[c - 0x80] : c;
}

// the length of the whole UTF-8 character at s (1 to 4), or 0 if it isn't one
static int Utf8_Len(const unsigned char *s)
{
	int n = s[0] < 0x80 ? 1 : (s[0] & 0xe0) == 0xc0 && s[0] >= 0xc2 ? 2 : (s[0] & 0xf0) == 0xe0 ? 3 : (s[0] & 0xf8) == 0xf0 && s[0] <= 0xf4 ? 4 : 0;
	for (int i = 1; i < n; i++) if ((s[i] & 0xc0) != 0x80) return 0;
	return n;
}

void kit_utf8_clean(const char *in, char *out, int size)
{
	const unsigned char *s = (const unsigned char *)in;
	int o = 0;
	if (size <= 0) return;
	while (*s) {
		int n = Utf8_Len(s);
		char buf[4];
		if (n) memcpy(buf, s, n);
		else {
			uint32_t c = kit_cp1252(*s);
			if (c < 0x800) { buf[0] = (char)(0xc0 | (c >> 6)); buf[1] = (char)(0x80 | (c & 0x3f)); n = 2; }
			else { buf[0] = (char)(0xe0 | (c >> 12)); buf[1] = (char)(0x80 | ((c >> 6) & 0x3f)); buf[2] = (char)(0x80 | (c & 0x3f)); n = 3; }
		}
		if (o + n >= size) break;
		memcpy(out + o, buf, n);
		o += n;
		s += Utf8_Len(s) ? (size_t)Utf8_Len(s) : 1;
	}
	out[o] = 0;
}
