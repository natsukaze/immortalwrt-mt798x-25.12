#define _GNU_SOURCE
#include "openfi_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ojson {
	char *buf;
	size_t len;
	size_t cap;
	int n;			/* 已写入的字段数 */
};

static void reserve(struct ojson *o, size_t need)
{
	if (o->len + need + 1 <= o->cap)
		return;
	size_t cap = o->cap ? o->cap : 256;

	while (cap < o->len + need + 1)
		cap *= 2;
	char *nb = realloc(o->buf, cap);

	if (!nb)
		return;			/* 内存不够就保持原样,宁可少写不要崩 */
	o->buf = nb;
	o->cap = cap;
}

static void put(struct ojson *o, const char *s, size_t n)
{
	reserve(o, n);
	if (!o->buf)
		return;
	memcpy(o->buf + o->len, s, n);
	o->len += n;
	o->buf[o->len] = '\0';
}

static void puts_(struct ojson *o, const char *s)
{
	put(o, s, strlen(s));
}

/* 写一个 JSON 字符串,正确转义(引号/反斜杠/控制字符) */
static void put_escaped(struct ojson *o, const char *s)
{
	puts_(o, "\"");
	for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
		switch (*p) {
		case '"':  puts_(o, "\\\""); break;
		case '\\': puts_(o, "\\\\"); break;
		case '\n': puts_(o, "\\n"); break;
		case '\r': puts_(o, "\\r"); break;
		case '\t': puts_(o, "\\t"); break;
		case '\b': puts_(o, "\\b"); break;
		case '\f': puts_(o, "\\f"); break;
		default:
			if (*p < 0x20) {
				/* 其余控制字符一律 \u00XX —— 裸控制字符会让 JSON 解析失败,
				   这一点以前吃过亏(状态文件读出来解析不了)。 */
				char tmp[8];

				snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
				puts_(o, tmp);
			} else {
				put(o, (const char *)p, 1);
			}
		}
	}
	puts_(o, "\"");
}

static void put_key(struct ojson *o, const char *k)
{
	if (!o || !k)
		return;
	if (o->n++)
		puts_(o, ",");
	put_escaped(o, k);
	puts_(o, ":");
}

static void ensure_open(struct ojson *o)
{
	if (o && !o->buf)
		puts_(o, "{");
}

struct ojson *oj_new(void)
{
	struct ojson *o = calloc(1, sizeof(*o));

	if (!o)
		return NULL;
	puts_(o, "{");
	return o;
}

void oj_str(struct ojson *o, const char *k, const char *v)
{
	ensure_open(o);
	if (!o || !k)
		return;
	put_key(o, k);
	if (!v)
		puts_(o, "null");
	else
		put_escaped(o, v);
}

void oj_int(struct ojson *o, const char *k, long v)
{
	char tmp[32];

	ensure_open(o);
	if (!o || !k)
		return;
	put_key(o, k);
	snprintf(tmp, sizeof(tmp), "%ld", v);
	puts_(o, tmp);
}

void oj_bool(struct ojson *o, const char *k, int v)
{
	ensure_open(o);
	if (!o || !k)
		return;
	put_key(o, k);
	puts_(o, v ? "true" : "false");
}

void oj_null(struct ojson *o, const char *k)
{
	ensure_open(o);
	if (!o || !k)
		return;
	put_key(o, k);
	puts_(o, "null");
}

void oj_raw(struct ojson *o, const char *k, const char *raw)
{
	ensure_open(o);
	if (!o || !k)
		return;
	put_key(o, k);
	puts_(o, raw ? raw : "null");
}

char *oj_render(struct ojson *o)
{
	if (!o)
		return NULL;
	puts_(o, "}");
	return o->buf ? strdup(o->buf) : NULL;
}

void oj_free(struct ojson *o)
{
	if (!o)
		return;
	free(o->buf);
	free(o);
}

void oj_emit(struct ojson *o)
{
	char *s;

	if (!o)
		return;
	s = oj_render(o);
	if (s) {
		fputs(s, stdout);
		fputc('\n', stdout);
		free(s);
	}
	oj_free(o);
}

int oj_fail(const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	struct ojson *o;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	o = oj_new();
	oj_bool(o, "ok", 0);
	oj_str(o, "error", msg);
	oj_emit(o);
	return 1;
}
