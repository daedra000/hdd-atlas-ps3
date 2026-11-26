/*
sfo.c: PARAM.SFO and TROPCONF.SFM (sfo.h).

PARAM.SFO: "\0PSF", a version, where the keys and the values start, and how
many entries; each entry (16 bytes, little-endian) gives its key's offset,
its format (0x0204 UTF-8, 0x0404 a 32-bit number), its length and its value's
offset. Everything's checked against the buffer's length: a damaged file
gives nothing, never a read past its end.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sfo.h"

static uint32_t Le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t Le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int sfo_get(const unsigned char *buf, size_t len, const char *key, char *out, int size)
{
	if (size <= 0) return 0;
	out[0] = 0;
	if (!buf || len < 20 || memcmp(buf, "\0PSF", 4)) return 0;
	uint32_t keys = Le32(buf + 8), vals = Le32(buf + 12), n = Le32(buf + 16);
	if (keys > len || vals > len || n > 4096 || 20 + (size_t)n * 16 > len) return 0;
	size_t klen = strlen(key);
	for (uint32_t i = 0; i < n; i++) {
		const unsigned char *e = buf + 20 + i * 16;
		size_t ko = (size_t)keys + Le16(e), vo = (size_t)vals + Le32(e + 12);
		uint32_t fmt = Le16(e + 2), vlen = Le32(e + 4);
		if (ko + klen + 1 > len || memcmp(buf + ko, key, klen + 1)) continue;
		if (vo > len || vlen > len - vo) return 0;
		if (fmt == 0x0404 && vlen >= 4) {
			snprintf(out, size, "%u", (unsigned)Le32(buf + vo));
			return 1;
		}
		size_t m = vlen;
		const void *z = memchr(buf + vo, 0, vlen);
		if (z) m = (const unsigned char *)z - (buf + vo);
		if (m >= (size_t)size) m = size - 1;
		memcpy(out, buf + vo, m);
		out[m] = 0;
		sfo_clean(out, size);
		return out[0] != 0;
	}
	return 0;
}

int sfo_trophy_title(const unsigned char *buf, size_t len, char *out, int size)
{
	static const char open[] = "<title-name>", close[] = "</title-name>";
	if (size <= 0) return 0;
	out[0] = 0;
	if (!buf) return 0;
	for (size_t i = 0; i + sizeof(open) - 1 <= len; i++) {
		if (memcmp(buf + i, open, sizeof(open) - 1)) continue;
		size_t a = i + sizeof(open) - 1, b = a;
		while (b + sizeof(close) - 1 <= len && memcmp(buf + b, close, sizeof(close) - 1)) b++;
		if (b + sizeof(close) - 1 > len) return 0;
		size_t m = b - a;
		if (m >= (size_t)size) m = size - 1;
		memcpy(out, buf + a, m);
		out[m] = 0;
		// (the few entities a name can hold)
		static const struct { const char *e; char c; } ents[] = {{"&amp;", '&'}, {"&apos;", '\''}, {"&quot;", '"'}, {"&lt;", '<'}, {"&gt;", '>'}};
		for (char *p = out; *p; p++)
			for (int k = 0; k < 5; k++) {
				size_t el = strlen(ents[k].e);
				if (!strncmp(p, ents[k].e, el)) { *p = ents[k].c; memmove(p + 1, p + el, strlen(p + el) + 1); break; }
			}
		sfo_clean(out, size);
		return out[0] != 0;
	}
	return 0;
}

void sfo_clean(char *s, int size)
{
	int n = (int)strnlen(s, size - 1);
	s[n] = 0;
	for (int i = 0; i < n; i++)
		if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) s[i] = ' ';
	// (a cut that split a character: back to its start)
	if (n && (s[n - 1] & 0x80)) {
		int k = n - 1;
		while (k > 0 && (s[k] & 0xc0) == 0x80) k--;
		int need = (s[k] & 0xe0) == 0xc0 ? 2 : (s[k] & 0xf0) == 0xe0 ? 3 : (s[k] & 0xf8) == 0xf0 ? 4 : 1;
		if (n - k < need) { n = k; s[n] = 0; }
	}
	char *p = s;
	while (*p == ' ') p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	n = (int)strlen(s);
	while (n && s[n - 1] == ' ') s[--n] = 0;
	// (runs of spaces, from line breaks: one)
	for (char *q = s; *q; q++)
		while (q[0] == ' ' && q[1] == ' ') memmove(q, q + 1, strlen(q + 1) + 1);
}
