/* 极简测试框架:够用、能看清哪条挂了 */
#ifndef OPENFI_T_H
#define OPENFI_T_H

#include <stdio.h>
#include <string.h>

static int t_pass, t_fail;

#define T_OK(cond) do { \
	if (cond) { t_pass++; } \
	else { t_fail++; printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define T_EQ_STR(got, want) do { \
	const char *_g = (got), *_w = (want); \
	if (_g && _w && !strcmp(_g, _w)) { t_pass++; } \
	else { t_fail++; printf("  ✗ %s:%d  期望 \"%s\" 实得 \"%s\"\n", \
	       __FILE__, __LINE__, _w ? _w : "(null)", _g ? _g : "(null)"); } \
} while (0)

#define T_EQ_INT(got, want) do { \
	long _g = (long)(got), _w = (long)(want); \
	if (_g == _w) { t_pass++; } \
	else { t_fail++; printf("  ✗ %s:%d  期望 %ld 实得 %ld\n", __FILE__, __LINE__, _w, _g); } \
} while (0)

#define T_SUMMARY() do { \
	printf("\n  通过 %d 项,失败 %d 项\n", t_pass, t_fail); \
	return t_fail ? 1 : 0; \
} while (0)

#endif
