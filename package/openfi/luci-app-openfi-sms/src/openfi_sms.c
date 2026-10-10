#define _GNU_SOURCE
#include "openfi_sms.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * GSM 03.38 默认字母表(0x00..0x7F)。
 * ★ 它不是 ASCII:0x00 是 '@'、0x24 是 '¤'、0x40 是 '¡'、0x5B 起是 Ä Ö Ñ Ü § …
 *   直接当 ASCII 处理,遇到这些位置就会串字符 —— 所以老老实实查表。
 * 0x1B 是转义前缀(扩展表),这里遇到就把下一个字符按扩展表查。
 */
static const char *GSM7[128] = {
	"@", "£", "$", "¥", "è", "é", "ù", "ì",
	"ò", "Ç", "\n", "Ø", "ø", "\r", "Å", "å",
	"Δ", "_", "Φ", "Γ", "Λ", "Ω", "Π", "Ψ",
	"Σ", "Θ", "Ξ", "\x1b", "Æ", "æ", "ß", "É",
	" ", "!", "\"", "#", "¤", "%", "&", "'",
	"(", ")", "*", "+", ",", "-", ".", "/",
	"0", "1", "2", "3", "4", "5", "6", "7",
	"8", "9", ":", ";", "<", "=", ">", "?",
	"¡", "A", "B", "C", "D", "E", "F", "G",
	"H", "I", "J", "K", "L", "M", "N", "O",
	"P", "Q", "R", "S", "T", "U", "V", "W",
	"X", "Y", "Z", "Ä", "Ö", "Ñ", "Ü", "§",
	"¿", "a", "b", "c", "d", "e", "f", "g",
	"h", "i", "j", "k", "l", "m", "n", "o",
	"p", "q", "r", "s", "t", "u", "v", "w",
	"x", "y", "z", "ä", "ö", "ñ", "ü", "à",
};

/*
 * 扩展表(前面加 0x1B 转义)。
 *
 * ★ 用【(下标, 字符) 对】来建表,而不是手排 128 个位置 ——
 *   手排我错了两次:第一次按 ASCII 猜下标(以为 { 在 0x7B),
 *   第二次排好了行但行内位置又错了 8。这种表手排 128 个格子,
 *   错一个就有一个字符编不进去或者编成别的字符,而且很难看出来。
 *
 * GSM 03.38 的扩展字符(下标是规范里定死的):
 *   0x0A 换页  0x14 ^  0x28 {  0x29 }  0x2F \
 *   0x3C [     0x3D ~  0x3E ]  0x40 |  0x65 €
 */
static const char *GSM7_EXT[128];

static const struct { int idx; const char *ch; } GSM7_EXT_DEF[] = {
	{ 0x0A, "\f" }, { 0x14, "^" },
	{ 0x28, "{" },    { 0x29, "}" }, { 0x2F, "\\" },
	{ 0x3C, "[" },    { 0x3D, "~" }, { 0x3E, "]" },
	{ 0x40, "|" },    { 0x65, "€" },
	{ -1, NULL }
};

static void gsm7_ext_init(void)
{
	static int done;

	if (done)
		return;
	for (int i = 0; GSM7_EXT_DEF[i].idx >= 0; i++)
		GSM7_EXT[GSM7_EXT_DEF[i].idx] = GSM7_EXT_DEF[i].ch;
	done = 1;
}

void sms_init(struct sms *s)
{
	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->dcs = SMS_DCS_7BIT;
	s->concat_total = 0;
	s->concat_seq = 0;
}

/* ---- 十六进制 ---- */

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int sms_hex2bytes(const char *hex, unsigned char *out, size_t outlen)
{
	size_t n = 0;
	int hi = -1;

	if (!hex || !out)
		return -1;
	for (; *hex; hex++) {
		int v;

		if (isspace((unsigned char)*hex))
			continue;
		v = hexval((unsigned char)*hex);
		if (v < 0)
			return -1;
		if (hi < 0) {
			hi = v;
		} else {
			if (n >= outlen)
				return -1;
			out[n++] = (unsigned char)((hi << 4) | v);
			hi = -1;
		}
	}
	if (hi >= 0)
		return -1;			/* 奇数个十六进制字符 */
	return (int)n;
}

void sms_bytes2hex(const unsigned char *in, size_t n, char *out, size_t outlen)
{
	static const char *H = "0123456789abcdef";
	size_t j = 0;

	if (!out || !outlen)
		return;
	for (size_t i = 0; i < n && j + 3 <= outlen; i++) {
		out[j++] = H[(in[i] >> 4) & 0xF];
		out[j++] = H[in[i] & 0xF];
	}
	out[j] = '\0';
}

void sms_swap_nibbles(char *s)
{
	for (; s && s[0] && s[1]; s += 2) {
		char t = s[0];

		s[0] = s[1];
		s[1] = t;
	}
}

/* ---- 地址 ---- */

/*
 * total_bytes = 整个地址字段占几个字节【含开头的"数字个数"字节】。
 *   字段布局:p[0] 数字个数,p[1] 类型(TON/NPI),p[2..total_bytes-1] 数字(半字节交换 BCD)
 * ★ 关键:PDU 里那个长度字节记的是【数字个数】而不是字节数
 *   (13 位号码写 0x0D,但只占 7 个字节)。一开始我按字节数用,
 *   结果短信中心字段多跳了 5 个字节,后面全部错位 —— 号码变成乱码。
 */
int sms_decode_addr(const unsigned char *p, int total_bytes, char *out, size_t outlen)
{
	int j = 0, ton;

	if (!p || !out || outlen < 2 || total_bytes < 2)
		return -1;

	ton = p[1];
	/* TON 在 bit 4..6:001 = 国际号码,000 = 本地/未知 */
	if ((ton & 0x70) == 0x10 && j + 1 < (int)outlen)
		out[j++] = '+';

	/* 数字从下标 2 开始:p[0] 是数字个数,p[1] 是类型字节。
	   从 1 开始的话会把 0x91 当成数字,号码前面多出 "19"。 */
	for (int i = 2; i < total_bytes && j + 2 < (int)outlen; i++) {
		int lo = p[i] & 0x0F;
		int hi = (p[i] >> 4) & 0x0F;

		if (lo <= 9)
			out[j++] = (char)('0' + lo);
		/* 0xF 是奇数位号码的填充,遇到就停 */
		if (hi <= 9)
			out[j++] = (char)('0' + hi);
	}
	out[j] = '\0';
	return 0;
}

int sms_encode_addr(const char *num, unsigned char *out, size_t outlen, int with_len)
{
	int n = 0, digits = 0, intl = 0;
	const char *p = num;


	if (!num || !out)
		return -1;
	if (*p == '+') {
		intl = 1;
		p++;
	}
	size_t off = with_len ? 1 : 0;

	if (off >= outlen)
		return -1;
	out[off] = intl ? 0x91 : 0x81;		/* TON/NPI:国际 / 国内 */
	n = (int)off + 1;

	const char *d = p;
	int ndig = 0;

	while (d[ndig] && isdigit((unsigned char)d[ndig]))
		ndig++;
	if (ndig == 0)
		return -1;

	for (int i = 0; i < ndig; i += 2) {
		int lo = d[i] - '0';
		int hi = (i + 1 < ndig) ? d[i + 1] - '0' : 0x0F;

		if ((size_t)n >= outlen)
			return -1;
		out[n++] = (unsigned char)((hi << 4) | lo);
		digits += (i + 1 < ndig) ? 2 : 1;
	}
	if (with_len)
		out[0] = (unsigned char)digits;
	return n;
}

/* ---- 7bit 打包/解包 ---- */

int sms_unpack_7bit(const unsigned char *in, int septets, char *out, size_t outlen)
{
	int oi = 0;

	if (!in || !out || outlen < 2)
		return -1;
	gsm7_ext_init();
	for (int i = 0; i < septets && oi + 4 < (int)outlen; i++) {
		int bitpos = i * 7;
		int bi = bitpos / 8;
		int bo = bitpos % 8;
		int v = in[bi] >> bo;

		if (bo > 1)
			v |= in[bi + 1] << (8 - bo);
		v &= 0x7F;

		if (v == 0x1B) {
			/* 扩展表:下一个 septet 是扩展字符 */
			i++;
			int bp2 = i * 7, bi2 = bp2 / 8, bo2 = bp2 % 8;
			int w = in[bi2] >> bo2;

			if (bo2 > 1)
				w |= in[bi2 + 1] << (8 - bo2);
			w &= 0x7F;
			const char *e = GSM7_EXT[w];

			if (e) {
				size_t l = strlen(e);

				memcpy(out + oi, e, l);
				oi += (int)l;
			}
			continue;
		}
		const char *g = GSM7[v];

		if (g) {
			size_t l = strlen(g);

			memcpy(out + oi, g, l);
			oi += (int)l;
		}
	}
	out[oi] = '\0';
	return oi;
}

int sms_pack_7bit(const char *text, unsigned char *out, size_t outlen, int *septets_out)
{
	int septets = 0;
	const char *t = text;

	if (!text || !out)
		return -1;
	gsm7_ext_init();
	memset(out, 0, outlen);

	/*
	 * ★ 按【UTF-8 字符】一个一个处理,不能按字节。
	 *   因为 GSM 默认表里有 ¤ ¡ Ä £ è 这类非 ASCII 字符,它们在 UTF-8 里
	 *   是两三个字节 —— 只比第一个字节永远匹配不上(这是我第一版的 bug)。
	 *   做法:拿表里的每一项去和输入当前位置做前缀比较,匹配就整段跳过。
	 */
	while (*t) {
		int code = -1, is_ext = 0;
		size_t adv = 1;

		for (int i = 0; i < 128; i++) {
			size_t l;

			if (!GSM7[i] || !GSM7[i][0])
				continue;
			l = strlen(GSM7[i]);
			if (strncmp(GSM7[i], t, l) == 0) {
				code = i;
				adv = l;
				break;
			}
		}
		if (code < 0) {
			for (int i = 0; i < 128; i++) {
				size_t l;

				if (!GSM7_EXT[i] || !GSM7_EXT[i][0])
					continue;
				l = strlen(GSM7_EXT[i]);
				if (strncmp(GSM7_EXT[i], t, l) == 0) {
					code = i;
					adv = l;
					is_ext = 1;
					break;
				}
			}
		}
		if (code < 0)
			return -1;		/* 这个字符 7 位表里没有(该走 UCS2)*/

		if (is_ext) {
			/* 先写 0x1B 转义,再写字符本身 */
			int bp = septets * 7, bi = bp / 8, bo = bp % 8;

			if ((size_t)bi + 1 >= outlen)
				return -1;
			out[bi] |= (unsigned char)(0x1B << bo);
			if (bo > 1)
				out[bi + 1] |= (unsigned char)(0x1B >> (8 - bo));
			septets++;
		}
		{
			int bp = septets * 7, bi = bp / 8, bo = bp % 8;

			if ((size_t)bi + 1 >= outlen)
				return -1;
			out[bi] |= (unsigned char)(code << bo);
			if (bo > 1)
				out[bi + 1] |= (unsigned char)(code >> (8 - bo));
			septets++;
		}
		t += adv;
	}
	if (septets_out)
		*septets_out = septets;
	return (septets * 7 + 7) / 8;
}

/* ---- UCS2 ---- */

int sms_ucs2_to_utf8(const unsigned char *in, int nbytes, char *out, size_t outlen)
{
	int oi = 0;

	if (!in || !out || outlen < 2)
		return -1;
	for (int i = 0; i + 1 < nbytes; i += 2) {
		unsigned int u = ((unsigned int)in[i] << 8) | in[i + 1];

		/* 代理对(emoji 之类的增补平面)*/
		if (u >= 0xD800 && u <= 0xDBFF && i + 3 < nbytes) {
			unsigned int lo = ((unsigned int)in[i + 2] << 8) | in[i + 3];

			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
		}
		if (u < 0x80) {
			if (oi + 2 > (int)outlen) break;
			out[oi++] = (char)u;
		} else if (u < 0x800) {
			if (oi + 3 > (int)outlen) break;
			out[oi++] = (char)(0xC0 | (u >> 6));
			out[oi++] = (char)(0x80 | (u & 0x3F));
		} else if (u < 0x10000) {
			if (oi + 4 > (int)outlen) break;
			out[oi++] = (char)(0xE0 | (u >> 12));
			out[oi++] = (char)(0x80 | ((u >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (u & 0x3F));
		} else {
			if (oi + 5 > (int)outlen) break;
			out[oi++] = (char)(0xF0 | (u >> 18));
			out[oi++] = (char)(0x80 | ((u >> 12) & 0x3F));
			out[oi++] = (char)(0x80 | ((u >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (u & 0x3F));
		}
	}
	out[oi] = '\0';
	return oi;
}

int sms_utf8_to_ucs2(const char *utf8, unsigned char *out, size_t outlen)
{
	size_t oi = 0;
	const unsigned char *p = (const unsigned char *)utf8;

	if (!utf8 || !out)
		return -1;
	while (*p && oi + 2 <= outlen) {
		unsigned int cp;
		int extra;

		if (*p < 0x80) {
			cp = *p++;
			extra = 0;
		} else if ((*p & 0xE0) == 0xC0) {
			cp = *p++ & 0x1F;
			extra = 1;
		} else if ((*p & 0xF0) == 0xE0) {
			cp = *p++ & 0x0F;
			extra = 2;
		} else if ((*p & 0xF8) == 0xF0) {
			cp = *p++ & 0x07;
			extra = 3;
		} else {
			p++;
			continue;
		}
		for (int i = 0; i < extra; i++) {
			if ((*p & 0xC0) != 0x80)
				break;
			cp = (cp << 6) | (*p++ & 0x3F);
		}
		if (cp < 0x10000) {
			out[oi++] = (unsigned char)(cp >> 8);
			out[oi++] = (unsigned char)(cp & 0xFF);
		} else {
			/* 需要代理对 */
			if (oi + 4 > outlen)
				break;
			cp -= 0x10000;
			unsigned int hi = 0xD800 + (cp >> 10);
			unsigned int lo = 0xDC00 + (cp & 0x3FF);

			out[oi++] = (unsigned char)(hi >> 8);
			out[oi++] = (unsigned char)(hi & 0xFF);
			out[oi++] = (unsigned char)(lo >> 8);
			out[oi++] = (unsigned char)(lo & 0xFF);
		}
	}
	return (int)oi;
}

/* ---- PDU 解码(SMS-DELIVER)---- */

/* PDU 时间戳:7 个字节的半字节交换 BCD,最后一字节是时区 */
static void decode_ts(const unsigned char *p, char *out, size_t outlen)
{
	char b[16];

	snprintf(b, sizeof(b), "%02x%02x%02x%02x%02x%02x%02x",
		 p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
	sms_swap_nibbles(b);
	/* b 现在形如 "26011015300040" — 前 12 位是时间,后 2 位是时区 */
	snprintf(out, outlen, "20%.2s-%.2s-%.2s %.2s:%.2s:%.2s",
		 b, b + 2, b + 4, b + 6, b + 8, b + 10);
}

int sms_decode_pdu(const char *hex, struct sms *s)
{
	unsigned char b[SMS_HEX_MAX / 2];
	int n, i = 0;
	int smsc_len, udlen, udh_len = 0;
	enum sms_dcs dcs;
	int ud_start;

	if (!hex || !s)
		return -1;
	sms_init(s);
	n = sms_hex2bytes(hex, b, sizeof(b));
	if (n < 10)
		return -1;

	/* ① 短信中心 */
	smsc_len = b[i++];			/* ★ 数字个数 */
	if (smsc_len > 0) {
		int bytes = 2 + (smsc_len + 1) / 2;   /* 长度字节 + 类型字节 + 数字字节 */

		if (i + bytes - 1 > n)
			return -1;
		sms_decode_addr(b + i - 1, bytes, s->sca, sizeof(s->sca));
		i += bytes - 1;
	}
	if (i >= n)
		return -1;

	/*
	 * ② 第一字节(PDU 类型)。
	 *   bit 0-1 = 消息类型指示(MTI):0=SMS-DELIVER,1=SMS-SUBMIT
	 *   bit 6   = 用户数据里有 UDH(长短信头)
	 * ★ 两种类型的字段布局【不一样】:
	 *     DELIVER: 号码 PID DCS 时间戳 UDL UD
	 *     SUBMIT : 消息参考号 号码 PID DCS [有效期] UDL UD
	 *   所以必须分开处理 —— 只按 DELIVER 解 SUBMIT 会整个错位。
	 *   支持 SUBMIT 的实际用处:发之前能核对"这条 PDU 里到底是什么内容"。
	 */
	int fo = b[i++];
	int has_udh = (fo & 0x40) != 0;
	int mti = fo & 0x03;
	int is_submit = (mti == 1);
	int has_vp = is_submit && ((fo & 0x18) != 0);

	if (is_submit) {
		if (i >= n)
			return -1;
		i++;				/* 消息参考号 */
	}

	/* ③ 对端号码(DELIVER 是发送方,SUBMIT 是接收方) */
	if (i >= n)
		return -1;
	int alen = b[i++];			/* ★ 数字个数 */
	{
		int bytes = 1 + (alen + 1) / 2;	/* 类型字节 + 数字字节 */
		int total = bytes + 1;		/* 含"数字个数"字节 */

		if (i + bytes > n)
			return -1;
		sms_decode_addr(b + i - 1, total, s->addr, sizeof(s->addr));
		i += bytes;
	}

	/* ④ PID / DCS */
	if (i + 2 > n)
		return -1;
	i++;					/* PID */
	int dcs_raw = b[i++];

	dcs = SMS_DCS_7BIT;
	if ((dcs_raw & 0x0C) == 0x08)
		dcs = SMS_DCS_UCS2;
	else if ((dcs_raw & 0x0C) == 0x04)
		dcs = SMS_DCS_8BIT;
	s->dcs = dcs;

	/* DELIVER 有时间戳(短信中心盖的);SUBMIT 没有,但可能带有效期 */
	if (is_submit) {
		if (has_vp) {
			/* 相对格式 1 字节 / 绝对格式 7 字节 */
			if (i >= n)
				return -1;
			int vp = b[i];

			i += (vp <= 0xFF && (fo & 0x18) == 0x10) ? 1 : 7;
			if (i > n)
				return -1;
		}
		s->ts[0] = '\0';
	} else {
		if (i + 7 > n)
			return -1;
		decode_ts(b + i, s->ts, sizeof(s->ts));
		i += 7;
	}

	/*
	 * ⑤ 用户数据长度(UDL)。
	 * ★ UDL 的含义随 DCS 变:7 位编码时它是【字符数(septet)】,
	 *   UCS2/8 位时才是【字节数】。
	 *   我第一版统一按字节处理,结果 7 位那条多解出一个字符 ——
	 *   多出来的那一位来自字节末尾的填充位(值为 0),
	 *   而 GSM 默认表里 0 正好是 '@',所以正文尾巴上会莫名多一个 @。
	 */
	if (i >= n)
		return -1;
	int udl = b[i++];
	ud_start = i;
	if (dcs == SMS_DCS_7BIT)
		udlen = (udl * 7 + 7) / 8;	/* 换算成字节数,供越界检查 */
	else
		udlen = udl;
	if (ud_start + udlen > n)
		udlen = n - ud_start;

	/* ⑥ UDH(长短信拼接信息)*/
	if (has_udh && udlen > 1) {
		udh_len = b[ud_start];
		if (udh_len + 1 <= udlen) {
			const unsigned char *u = b + ud_start + 1;

			/* 00 03 <ref> <total> <seq>  —— 8 位参考号 */
			if (udh_len >= 5 && u[0] == 0x00 && u[1] == 0x03) {
				s->concat_ref = u[2];
				s->concat_total = u[3];
				s->concat_seq = u[4];
			/* 08 04 <ref hi> <ref lo> <total> <seq> —— 16 位参考号 */
			} else if (udh_len >= 6 && u[0] == 0x08 && u[1] == 0x04) {
				s->concat_ref = (u[2] << 8) | u[3];
				s->concat_total = u[4];
				s->concat_seq = u[5];
			}
		}
	}

	/*
	 * ⑦ 正文起点。
	 * ★ UDH 的第一个字节是 UDHL(它自己之后还有多少字节),
	 *   所以整段 UDH 占 udh_len + 1 个字节 —— 正文从【再下一个字节】开始。
	 *   少加这个 1,正文就整体前移一字节,中文(UCS2)会解成乱码。
	 */
	const unsigned char *ud = b + ud_start + (udh_len ? udh_len + 1 : 0);

	if (dcs == SMS_DCS_UCS2) {
		sms_ucs2_to_utf8(ud, udlen - udh_len, s->text, sizeof(s->text));
	} else if (dcs == SMS_DCS_7BIT) {
		/*
		 * ★ 7 位正文的字符数【直接就是 UDL】,不要拿字节数去反推 ——
		 *   反推会把末尾填充位算成一个字符。
		 *   带 UDH 时,UDH(含它自己的长度字节)也按 septet 占位,
		 *   并且要补一个填充位让正文落在字节边界上,
		 *   所以 UDH 折算成 ceil((udh_len+1)*8/7) 个 septet,从 UDL 里扣掉。
		 */
		int sept = udl;

		if (udh_len) {
			int udh_sept = ((udh_len + 1) * 8 + 6) / 7;

			sept -= udh_sept;
			if (sept < 0)
				sept = 0;
		}
		/* 再按实际可读字节数兜一次上限,防止越界 */
		{
			int cap = udlen * 8 / 7;

			if (sept > cap)
				sept = cap;
		}
		sms_unpack_7bit(ud, sept, s->text, sizeof(s->text));
	} else {
		/* 8 位数据:当二进制处理,转成可见的十六进制 */
		sms_bytes2hex(ud, (size_t)(udlen - udh_len), s->text, sizeof(s->text));
	}

	s->valid = 1;
	return 0;
}

/* ---- PDU 编码(SMS-SUBMIT)---- */

/*
 * 注:SMS-SUBMIT 的第一字节设为 0x01(不带有效期 VP),
 * 所以不需要时间戳字段 —— 那是短信中心收到之后自己盖的。
 */

int sms_encode_pdu(const char *sca, const char *addr, const char *text, int dcs,
		   int concat_total, int concat_seq, int concat_ref,
		   char *out, size_t outlen)
{
	unsigned char b[SMS_HEX_MAX / 2];
	int n = 0;
	int use_ucs2;
	int udh_bytes = 0;

	if (!addr || !text || !out || outlen < 4)
		return -1;

	/* 自动选编码:纯 GSM7 能表达就用 7 位,否则 UCS2 */
	{
		unsigned char tmp[512];
		int sept = 0;

		if (dcs < 0) {
			use_ucs2 = (sms_pack_7bit(text, tmp, sizeof(tmp), &sept) < 0);
		} else {
			use_ucs2 = (dcs == SMS_DCS_UCS2);
		}
	}

	/* ① 短信中心:空则写 00(表示"用模组里存的默认中心")*/
	if (sca && *sca) {
		int r = sms_encode_addr(sca, b + n, sizeof(b) - (size_t)n, 1);

		if (r < 0)
			return -1;
		n += r;
	} else {
		b[n++] = 0x00;
	}

	/* ② 第一字节:0x01 = SMS-SUBMIT,无有效期;带 UDH 时置 0x40 */
	int fo_pos = n;

	b[n++] = 0x01;

	/* ③ 消息参考号 */
	b[n++] = 0x00;

	/* ④ 对方号码 */
	{
		int r = sms_encode_addr(addr, b + n, sizeof(b) - (size_t)n, 1);

		if (r < 0)
			return -1;
		n += r;
	}

	/* ⑤ PID */
	b[n++] = 0x00;

	/* ⑥ DCS */
	b[n++] = use_ucs2 ? 0x08 : 0x00;

	/* ⑦ 用户数据长度 + 内容 */
	int udl_pos = n;

	n++;					/* 先占位 */

	/* UDH */
	if (concat_total > 1) {
		unsigned char *u = b + n;

		u[0] = 0x05;			/* UDHL */
		u[1] = 0x00;
		u[2] = 0x03;
		u[3] = (unsigned char)(concat_ref & 0xFF);
		u[4] = (unsigned char)concat_total;
		u[5] = (unsigned char)concat_seq;
		udh_bytes = 6;
		n += udh_bytes;
		b[fo_pos] |= 0x40;		/* UDHI */
	}

	if (use_ucs2) {
		int r = sms_utf8_to_ucs2(text, b + n, sizeof(b) - (size_t)n);

		if (r < 0)
			return -1;
		n += r;
		/* UDL 是字节数 */
		b[udl_pos] = (unsigned char)(n - udl_pos - 1);
	} else {
		int sept = 0;
		int r = sms_pack_7bit(text, b + n, sizeof(b) - (size_t)n, &sept);

		if (r < 0)
			return -1;
		n += r;
		/*
		 * ★ 7 位编码时 UDL 记的是【字符数】,不是字节数。
		 *   带 UDH 时还要把 UDH 折算成 septet 加进去
		 *   (UDH 按 7 位算并补一个填充位,所以是 (udh_bytes*8+6)/7)。
		 */
		b[udl_pos] = (unsigned char)(sept + (udh_bytes ? (udh_bytes * 8 + 6) / 7 : 0));
	}

	sms_bytes2hex(b, (size_t)n, out, outlen);
	return 0;
}

int sms_count_segments(const char *text, int use_ucs2)
{
	if (!text)
		return 0;
	if (use_ucs2) {
		/* UCS2:单条 70 字符,长短信每条 67 字符(UDH 占 6 字节 = 3 字符)*/
		size_t chars = 0;
		const unsigned char *p = (const unsigned char *)text;

		while (*p) {
			if (*p < 0x80) p++;
			else if ((*p & 0xE0) == 0xC0) p += 2;
			else if ((*p & 0xF0) == 0xE0) p += 3;
			else p += 4;
			chars++;
		}
		if (chars <= 70)
			return 1;
		return (int)((chars + 66) / 67);
	} else {
		size_t n = strlen(text);

		if (n <= 160)
			return 1;
		return (int)((n + 152) / 153);
	}
}
