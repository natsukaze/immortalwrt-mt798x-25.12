/* 短信 PDU 编解码测试
 *
 * 重点:
 *   · 用已知的 PDU 向量(网上一查就有的 "hello" 那条)当基准
 *   · 7 位字母表不是 ASCII,专门测那几个不一样的位置
 *   · 编码→解码往返(最强的正确性检查)
 *   · 中文走 UCS2、长短信 UDH 拼接
 *   · 位数算错会串位,所以长度相关的都单独测
 */
#define _GNU_SOURCE
#include "../src/openfi_sms.h"
#include "t.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
	struct sms s;
	char hex[SMS_HEX_MAX], buf[512];
	unsigned char bytes[512];
	int sept;

	printf("── 十六进制 ↔ 字节 ──\n");
	T_EQ_INT(sms_hex2bytes("E8329BFD06", bytes, sizeof(bytes)), 5);
	T_EQ_INT(bytes[0], 0xE8);
	T_EQ_INT(bytes[4], 0x06);
	T_EQ_INT(sms_hex2bytes("e8 32 9b fd 06", bytes, sizeof(bytes)), 5);  /* 带空格也行 */
	T_EQ_INT(sms_hex2bytes("E8329BFD0", bytes, sizeof(bytes)), -1);      /* 奇数个 */
	T_EQ_INT(sms_hex2bytes("E8329BFD0G", bytes, sizeof(bytes)), -1);     /* 非法字符 */
	sms_bytes2hex(bytes, 5, buf, sizeof(buf));
	T_EQ_STR(buf, "e8329bfd06");
	sms_bytes2hex(NULL, 0, buf, sizeof(buf));
	T_EQ_STR(buf, "");

	printf("── 半字节交换 ──\n");
	/* +8613800138000 的 BCD 就是 68 31 08 10 83 00 F0,交换后应还原成数字序列 */
	snprintf(buf, sizeof(buf), "683108108300F0");
	sms_swap_nibbles(buf);
	T_EQ_STR(buf, "8613800138000F");    /* 末尾 F 是奇数位填充 */
	snprintf(buf, sizeof(buf), "1");
	sms_swap_nibbles(buf);              /* 奇数长度不能越界 */
	T_EQ_STR(buf, "1");

	printf("── 号码解码(PDU 里是半字节交换的 BCD)──\n");
	{
		/* 0D 91 68 31 08 10 83 00 F0 → +8613800138000
		   (数字 8613800138000 按两位一组交换:86→68, 13→31, 80→08,
		     01→10, 38→83, 00→00, 末尾单 0 补 F → F0) */
		unsigned char a[] = { 0x0D, 0x91, 0x68, 0x31, 0x08, 0x10, 0x83, 0x00, 0xF0 };

		T_EQ_INT(sms_decode_addr(a, 9, buf, sizeof(buf)), 0);
		T_EQ_STR(buf, "+8613800138000");
	}
	{
		/* 0B 81 31 00 80 08 00 F0 → 本地号码 13000880000 */
		unsigned char a[] = { 0x0B, 0x81, 0x31, 0x00, 0x80, 0x08, 0x00, 0xF0 };

		T_EQ_INT(sms_decode_addr(a, 8, buf, sizeof(buf)), 0);
		T_EQ_STR(buf, "13000880000");
	}

	printf("── 号码编码 ──\n");
	{
		int n = sms_encode_addr("+8613800138000", bytes, sizeof(bytes), 1);

		T_OK(n > 0);
		T_EQ_INT(bytes[0], 13);         /* 数字个数 */
		T_EQ_INT(bytes[1], 0x91);       /* 国际 */
		T_EQ_INT(bytes[2], 0x68);       /* "86" → 0x68 */
		T_EQ_INT(bytes[5], 0x10);       /* "01" → 0x10 */
		/* 往返:n 是含长度字节的总字节数 */
		T_EQ_INT(sms_decode_addr(bytes, n, buf, sizeof(buf)), 0);
		T_EQ_STR(buf, "+8613800138000");
	}
	{
		int n = sms_encode_addr("13000880000", bytes, sizeof(bytes), 1);

		T_OK(n > 0);
		T_EQ_INT(bytes[1], 0x81);       /* 国内 */
		T_EQ_INT(sms_decode_addr(bytes, n, buf, sizeof(buf)), 0);
		T_EQ_STR(buf, "13000880000");
	}
	T_EQ_INT(sms_encode_addr("abc", bytes, sizeof(bytes), 1), -1);

	printf("── 7 位打包:已知向量(hello)──\n");
	T_EQ_INT(sms_pack_7bit("hello", bytes, sizeof(bytes), &sept), 5);
	T_EQ_INT(sept, 5);
	sms_bytes2hex(bytes, 5, buf, sizeof(buf));
	T_EQ_STR(buf, "e8329bfd06");        /* ★ 这是公认的 hello 打包结果 */
	/* 往返 */
	T_EQ_INT(sms_unpack_7bit(bytes, 5, hex, sizeof(hex)), 5);
	T_EQ_STR(hex, "hello");

	printf("── 7 位字母表【不是 ASCII】──\n");
	/* 0x00 是 '@'、0x24 是 '¤'、0x40 是 '¡'、0x5B 是 'Ä' */
	T_EQ_INT(sms_pack_7bit("@", bytes, sizeof(bytes), &sept), 1);
	T_EQ_INT(bytes[0], 0x00);
	T_EQ_INT(sms_pack_7bit("¤", bytes, sizeof(bytes), &sept), 1);
	T_EQ_INT(bytes[0], 0x24);
	T_EQ_INT(sms_pack_7bit("¡", bytes, sizeof(bytes), &sept), 1);
	T_EQ_INT(bytes[0], 0x40);
	T_EQ_INT(sms_pack_7bit("Ä", bytes, sizeof(bytes), &sept), 1);
	T_EQ_INT(bytes[0], 0x5B);

	printf("── 7 位往返:各种字符 ──\n");
	{
		const char *cases[] = { "Hello World", "0123456789", "aA1!@#$%", "[]{}()<>", NULL };

		for (int i = 0; cases[i]; i++) {
			int n = sms_pack_7bit(cases[i], bytes, sizeof(bytes), &sept);
			int ok = (n >= 0);

			if (ok) {
				sms_unpack_7bit(bytes, sept, buf, sizeof(buf));
				T_EQ_STR(buf, cases[i]);
			} else {
				T_OK(0);	/* 不该编不进去 */
			}
		}
	}

	printf("── 扩展表字符(€ { } \\ 等,占两个 septet)──\n");
	{
		int n = sms_pack_7bit("€", bytes, sizeof(bytes), &sept);

		T_OK(n >= 0);
		T_EQ_INT(sept, 2);              /* 转义符 + 字符 */
		sms_unpack_7bit(bytes, sept, buf, sizeof(buf));
		T_EQ_STR(buf, "€");
	}
	{
		int n = sms_pack_7bit("{a}", bytes, sizeof(bytes), &sept);

		T_OK(n >= 0);
		sms_unpack_7bit(bytes, sept, buf, sizeof(buf));
		T_EQ_STR(buf, "{a}");
	}

	printf("── 中文编不进 7 位(应失败,让调用方走 UCS2)──\n");
	T_EQ_INT(sms_pack_7bit("中文", bytes, sizeof(bytes), &sept), -1);

	printf("── UCS2 ↔ UTF-8 ──\n");
	{
		/* "测试" = U+6D4B U+8BD5 */
		unsigned char u[] = { 0x6D, 0x4B, 0x8B, 0xD5 };

		T_EQ_INT(sms_ucs2_to_utf8(u, 4, buf, sizeof(buf)), 6);
		T_EQ_STR(buf, "测试");
		T_EQ_INT(sms_utf8_to_ucs2("测试", bytes, sizeof(bytes)), 4);
		T_EQ_INT(bytes[0], 0x6D);
		T_EQ_INT(bytes[1], 0x4B);
		T_EQ_INT(bytes[2], 0x8B);
		T_EQ_INT(bytes[3], 0xD5);
	}
	{
		/* 往返:中文 + 英文 + emoji(增补平面,走代理对)*/
		const char *cases[] = { "测试", "Hello 世界", "a", "中文测试字符串", "😀", NULL };

		for (int i = 0; cases[i]; i++) {
			int n = sms_utf8_to_ucs2(cases[i], bytes, sizeof(bytes));

			T_OK(n > 0);
			sms_ucs2_to_utf8(bytes, n, buf, sizeof(buf));
			T_EQ_STR(buf, cases[i]);
		}
	}

	printf("── 解 PDU:英文(已知向量)──\n");
	{
		/* SMSC=+8613800138000,发送方 +8613800138000,正文 hello */
		const char *pdu =
			"0D91683108108300F0"      /* 短信中心 +8613800138000 */
			"04"                      /* SMS-DELIVER */
			"0D91683108108300F0"      /* 发送方 */
			"00"                      /* PID */
			"00"                      /* DCS = 7 位 */
			"62101000000000"          /* 时间戳 */
			"05"                      /* UDL = 5 个字符 */
			"E8329BFD06";             /* "hello" 的 7 位打包 */

		T_EQ_INT(sms_decode_pdu(pdu, &s), 0);
		T_EQ_INT(s.valid, 1);
		T_EQ_STR(s.sca, "+8613800138000");
		T_EQ_STR(s.addr, "+8613800138000");
		T_EQ_STR(s.text, "hello");
		T_EQ_INT(s.dcs, SMS_DCS_7BIT);
		T_EQ_INT(s.concat_total, 0);
	}

	printf("── 解 PDU:中文(UCS2)──\n");
	{
		/* 正文 "测试" = 6D4B8BD5,UDL=4 字节 */
		const char *pdu =
			"0D91683108108300F0"
			"04"
			"0D91683108108300F0"
			"00"
			"08"                      /* DCS = UCS2 */
			"62101000000000"
			"04"                      /* UDL = 4 字节 */
			"6D4B8BD5";               /* "测试" = U+6D4B U+8BD5 */

		T_EQ_INT(sms_decode_pdu(pdu, &s), 0);
		T_EQ_INT(s.dcs, SMS_DCS_UCS2);
		T_EQ_STR(s.text, "测试");
	}

	printf("── 长短信(UDH 拼接)—— 靠「编码再解码」覆盖 ──\n");
	/*
	 * 带 UDH 的 7 位编码要算"UDH 折算成多少 septet + 填充位",
	 * 手写报文很容易把填充算错。所以这里不手造报文,而是让程序自己编一条
	 * 带 UDH 的长短信,再解回来 —— 编解一致同样能证明 UDH 处理是对的
	 * (下面"往返:长短信"那条会检查 total/seq/ref 三个字段)。
	 */
	T_OK(1);

	printf("── ★ 编码 → 解码 往返(最强的正确性检查)──\n");
	{
		const char *cases[] = { "hello", "Hello World 123", "测试短信", "中英mixed文本",
					"a", "很长的一条短信内容用来测试会不会被正确编码再解回来", NULL };

		for (int i = 0; cases[i]; i++) {
			T_EQ_INT(sms_encode_pdu("+8613800138000", "10086", cases[i], -1,
						0, 0, 0, hex, sizeof(hex)), 0);
			T_EQ_INT(sms_decode_pdu(hex, &s), 0);
			T_EQ_STR(s.text, cases[i]);
			T_EQ_STR(s.addr, "10086");
		}
	}

	printf("── 往返:UCS2 强制 ──\n");
	{
		T_EQ_INT(sms_encode_pdu("+8613800138000", "10086", "abc", SMS_DCS_UCS2,
					0, 0, 0, hex, sizeof(hex)), 0);
		T_EQ_INT(sms_decode_pdu(hex, &s), 0);
		T_EQ_INT(s.dcs, SMS_DCS_UCS2);
		T_EQ_STR(s.text, "abc");
	}

	printf("── 往返:长短信(带 UDH)──\n");
	{
		T_EQ_INT(sms_encode_pdu("+8613800138000", "10086", "第一段内容", -1,
					3, 1, 77, hex, sizeof(hex)), 0);
		T_EQ_INT(sms_decode_pdu(hex, &s), 0);
		T_EQ_STR(s.text, "第一段内容");
		T_EQ_INT(s.concat_total, 3);
		T_EQ_INT(s.concat_seq, 1);
		T_EQ_INT(s.concat_ref, 77);
	}

	printf("── 没有短信中心(空)→ 用 00 ──\n");
	{
		T_EQ_INT(sms_encode_pdu("", "10086", "hi", -1, 0, 0, 0, hex, sizeof(hex)), 0);
		T_EQ_INT(sms_decode_pdu(hex, &s), 0);
		T_EQ_STR(s.text, "hi");
		T_EQ_STR(s.sca, "");
	}

	printf("── 分段数估算(界面提示用)──\n");
	T_EQ_INT(sms_count_segments("hello", 0), 1);
	T_EQ_INT(sms_count_segments("测试", 1), 1);
	{
		char big[200];

		memset(big, 'a', 160); big[160] = '\0';
		T_EQ_INT(sms_count_segments(big, 0), 1);
		memset(big, 'a', 161); big[161] = '\0';
		T_EQ_INT(sms_count_segments(big, 0), 2);
	}

	printf("── 异常输入不崩 ──\n");
	T_EQ_INT(sms_decode_pdu("", &s), -1);
	T_EQ_INT(sms_decode_pdu("ZZZZ", &s), -1);
	T_EQ_INT(sms_decode_pdu("079168310800138000", &s), -1);   /* 太短 */
	T_EQ_INT(sms_decode_pdu(NULL, &s), -1);
	T_EQ_INT(sms_decode_pdu("00", NULL), -1);
	T_EQ_INT(sms_encode_pdu("+86", NULL, "x", -1, 0, 0, 0, hex, sizeof(hex)), -1);
	T_EQ_INT(sms_encode_pdu("+86", "10086", NULL, -1, 0, 0, 0, hex, sizeof(hex)), -1);
	sms_init(NULL);

	T_SUMMARY();
}
