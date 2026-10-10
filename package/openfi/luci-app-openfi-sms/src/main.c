/*
 * openfi-sms —— OpenFi 6C 短信收发
 *
 * 用法:
 *   openfi-sms list [N]         列出短信(默认 50 条,新的在前)JSON
 *   openfi-sms read <id>        看一条(顺带标为已读)
 *   openfi-sms send <号码> <内容>  发一条
 *   openfi-sms delete <id>      删一条
 *   openfi-sms delete all       全删
 *   openfi-sms unread           未读条数(纯数字,给界面)
 *   openfi-sms collect          后台收短信(procd 拉起)
 *   openfi-sms selftest         自检
 *
 * 设计:
 *   · 短信用"一条一个文件"存(/etc/openfi-sms),掉电不坏
 *   · 收:定时问模组要【未读】短信,解出 PDU 存本地,然后从模组里删掉,
 *        这样不会把模组的存储塞满;不用 URC 是因为 URC 可能丢
 *   · 发:自己编 PDU 再走 AT+CMGS —— 不依赖模组的中文编码能力
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "openfi_at.h"
#include "openfi_at_io.h"
#include "openfi_json.h"
#include "openfi_log.h"
#include "openfi_sms.h"
#include "openfi_sms_store.h"
#include "openfi_sysfs.h"
#include "openfi_uci.h"

static volatile sig_atomic_t g_stop;

static void on_term(int sig) { (void)sig; g_stop = 1; }

static void copy_str(char *dst, size_t n, const char *src)
{
	size_t l;

	if (!dst || !n)
		return;
	if (!src)
		src = "";
	l = strlen(src);
	if (l >= n)
		l = n - 1;
	memcpy(dst, src, l);
	dst[l] = '\0';
}

/* ---------------- 模组侧 ---------------- */

struct sms_cfg {
	char at_port[128];
	char sca[64];			/* 短信中心;空 = 用模组里存的 */
	int  baud;
	int  timeout_ms;
	int  period;
};

static void cfg_load(struct sms_cfg *c)
{
	char *p = ouci_get("openfi_sms", "sms", "at_port");

	memset(c, 0, sizeof(*c));
	if (p && *p) {
		copy_str(c->at_port, sizeof(c->at_port), p);
		free(p);
	} else {
		free(p);
		/* 没配就跟模组包用同一个约定 */
		p = ouci_get("openfi_modem", "modem", "at_port");
		copy_str(c->at_port, sizeof(c->at_port), (p && *p) ? p : "/dev/ttyUSB2");
		free(p);
	}
	p = ouci_get("openfi_sms", "sms", "sca");
	copy_str(c->sca, sizeof(c->sca), p ? p : "");
	free(p);
	c->baud = (int)ouci_get_int("openfi_sms", "sms", "baud", 115200);
	c->timeout_ms = (int)ouci_get_int("openfi_sms", "sms", "timeout_ms", 2000);
	c->period = (int)ouci_get_int("openfi_sms", "sms", "period", 20);
	if (c->period < 10)
		c->period = 10;
}

static int open_port(const struct sms_cfg *c, struct at_port *p)
{
	return at_open(p, c->at_port, c->baud);
}

/* ---------------- 收 ---------------- */

/*
 * 把一批 "AT+CMGL" 的输出解析成短信并入库。
 * 模组返回的是【一行索引+状态,一行 PDU】交替:
 *   +CMGL: 1,"REC UNREAD","+8613800138000",,"26/10/10,15:30:12+32"
 *   0791683108108300F0040D91683108108300F000006210100000000005E8329BFD06
 * 返回入库条数。
 */
static int import_cmgl(const char *resp)
{
	int imported = 0;
	const char *p = resp;
	int pending_index = -1;

	while (p && *p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char line[1024];

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';
		/* 去尾部 \r 与空白 */
		for (size_t i = strlen(line); i && (line[i-1] == '\r' ||
						    line[i-1] == ' '); i--)
			line[i-1] = '\0';

		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;

		if (strncmp(s, "+CMGL:", 6) == 0) {
			/* 记下索引,后面那行 PDU 属于它 */
			pending_index = (int)strtol(s + 6, NULL, 10);
		} else if (pending_index >= 0 && *s && strspn(s, "0123456789abcdefABCDEF") == strlen(s)
			   && strlen(s) > 20) {
			/* 这就是一条 PDU */
			struct sms_msg m;

			memset(&m, 0, sizeof(m));
			if (sms_decode_pdu(s, &m.pdu) == 0) {
				m.received_at = (long)time(NULL);
				m.read = 0;
				if (sms_store_save(&m) == 0) {
					imported++;
					olog_info("收到短信 来自 %s:%s", m.pdu.addr, m.pdu.text);
					/* 存下来之后把模组里这条删掉,别把模组塞满 */
				}
			}
			pending_index = -1;
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	return imported;
}

/* 拉一次未读短信 */
static int fetch_unread(const struct sms_cfg *c)
{
	struct at_port port;
	struct at_resp r;
	char all[16384] = "";
	int imported = 0, total_on_modem = 0;
	int indices[64], nidx = 0;

	if (open_port(c, &port) != 0) {
		olog_warn("打不开 %s", c->at_port);
		return -1;
	}

	/* 先把所有未读列出来 */
	if (at_cmd(&port, "AT+CMGL=\"REC UNREAD\"", c->timeout_ms * 3, &r) != 0) {
		at_close(&port);
		return -1;
	}
	for (int i = 0; i < r.n && strlen(all) + strlen(r.lines[i]) + 2 < sizeof(all); i++) {
		strcat(all, r.lines[i]);
		strcat(all, "\n");
	}
	imported = import_cmgl(all);

	/* 记下模组上未读短信的索引,导入后删掉(释放模组存储)*/
	{
		const char *p = all;

		while ((p = strstr(p, "+CMGL:"))) {
			int idx = (int)strtol(p + 6, NULL, 10);

			if (nidx < (int)(sizeof(indices) / sizeof(indices[0])))
				indices[nidx++] = idx;
			total_on_modem++;
			p += 6;
		}
	}
	if (nidx > 0) {
		char cmd[32];

		snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", indices[0]);
		/* 逐条删;批量范围语法各家不一,逐条最保险 */
		for (int i = 0; i < nidx; i++) {
			snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", indices[i]);
			at_cmd(&port, cmd, c->timeout_ms, &r);
		}
	}

	/* 顺便把短信中心号读出来(发的时候要用)*/
	if (!c->sca[0]) {
		if (at_cmd(&port, "AT+CSCA?", c->timeout_ms, &r) == 0) {
			const char *l = at_find(&r, "+CSCA:");

			if (l) {
				char fields[4][64];

				if (at_csv(l, fields, 4) >= 1) {
					/* 记到配置里,下次不用再问 */
					ouci_set("openfi_sms", "sms", "sca", fields[0]);
				}
			}
		}
	}

	at_close(&port);
	return imported;
}

/* ---------------- 发 ---------------- */

static int send_sms(const struct sms_cfg *c, const char *num, const char *text)
{
	struct at_port port;
	struct at_resp r;
	char pdu[SMS_HEX_MAX];
	struct sms_msg m;
	int segs = sms_count_segments(text, 0);
	int rc = 0;

	if (open_port(c, &port) != 0)
		return oj_fail("打不开串口 %s", c->at_port);

	/* 先设文本模式,免得模组把我们要发的 PDU 当命令解释 */
	at_cmd(&port, "AT+CMGF=0", c->timeout_ms, &r);

	int seg = 1;
	int ref = (int)(time(NULL) & 0xFF);
	const char *remain = text;

	/* ★ 长短信按 67/153 个字符切段,每段带 UDH 参考号,收端才能拼起来 */
	while (*remain && rc == 0) {
		int maxchars = sms_count_segments(remain, 0) > 1 ? 67 : 1000;
		char chunk[SMS_TEXT_MAX];
		int take = 0;

		/* 按 UTF-8 字符数切(不能按字节切,否则中文会被切成半个字)*/
		int chars = 0;
		const unsigned char *q = (const unsigned char *)remain;

		while (*q && chars < (segs > 1 ? 67 : 160)) {
			int l = (*q < 0x80) ? 1 : ((*q & 0xE0) == 0xC0) ? 2 :
				((*q & 0xF0) == 0xE0) ? 3 : 4;

			for (int i = 0; i < l && q[i]; i++)
				chunk[take++] = (char)q[i];
			q += l;
			chars++;
		}
		chunk[take] = '\0';
		remain = (const char *)q;

		char cmd[64];

		if (sms_encode_pdu(c->sca, num, chunk, -1,
				   segs > 1 ? segs : 0, segs > 1 ? seg : 0, ref,
				   pdu, sizeof(pdu)) != 0) {
			rc = oj_fail("PDU 编码失败");
			break;
		}

		snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", (int)(strlen(pdu) / 2 - 1));
		if (at_cmd(&port, cmd, c->timeout_ms, &r) != 0 ||
		    !strstr(at_result_str(r.result), "OK")) {
			/* 模组会用 "> " 提示要 PDU;这里不强求,直接发*/
		}
		if (at_cmd(&port, pdu, c->timeout_ms * 3, &r) != 0 ||
		    r.result != AT_RES_OK) {
			rc = oj_fail("发送失败:%s", at_result_str(r.result));
			break;
		}
		seg++;
		(void)maxchars;
	}

	at_close(&port);

	if (rc == 0) {
		/* 存一份到"已发送" */
		memset(&m, 0, sizeof(m));
		copy_str(m.pdu.addr, sizeof(m.pdu.addr), num);
		copy_str(m.pdu.text, sizeof(m.pdu.text), text);
		m.pdu.dcs = SMS_DCS_UCS2;
		m.outgoing = 1;
		m.read = 1;
		m.received_at = (long)time(NULL);
		sms_store_save(&m);

		struct ojson *o = oj_new();

		oj_bool(o, "ok", 1);
		oj_str(o, "to", num);
		oj_int(o, "segments", segs);
		oj_emit(o);
	}
	return rc;
}

/* ---------------- 输出 ---------------- */

static void emit_msg_json(const struct sms_msg *m, int with_text)
{
	struct ojson *o = oj_new();

	oj_str(o, "id", m->id);
	oj_str(o, "addr", m->pdu.addr);
	oj_str(o, "ts", m->pdu.ts);
	oj_bool(o, "read", m->read);
	oj_bool(o, "outgoing", m->outgoing);
	oj_int(o, "received_at", m->received_at);
	if (with_text)
		oj_str(o, "text", m->pdu.text);
	else
		oj_int(o, "text_len", (long)strlen(m->pdu.text));
	if (m->pdu.concat_total > 1)
		oj_int(o, "part", m->pdu.concat_seq);
	oj_emit(o);
}

static void usage(void)
{
	fprintf(stderr,
		"用法: openfi-sms <命令>\n"
		"  list [N]          列出短信(默认 50,新在前)\n"
		"  read <id>         看一条并标为已读\n"
		"  send <号码> <内容> 发一条\n"
		"  delete <id>|all   删除\n"
		"  unread            未读条数\n"
		"  collect           后台收短信\n"
		"  selftest          自检\n");
}

int main(int argc, char **argv)
{
	struct sms_cfg cfg;

	olog_init("openfi-sms");
	{
		const char *e = getenv("OPENFI_SYSFS_ROOT");

		if (e && *e)
			osys_set_root(e);
		e = getenv("OPENFI_UCI_ROOT");
		if (e && *e)
			ouci_set_root(e);
		e = getenv("OPENFI_SMS_DIR");
		if (e && *e)
			sms_store_set_dir(e);
	}
	cfg_load(&cfg);
	sms_store_init();

	if (argc < 2) {
		usage();
		return 1;
	}
	const char *act = argv[1];

	if (!strcmp(act, "list")) {
		int max = (argc > 2) ? atoi(argv[2]) : 50;
		static struct sms_msg all[512];
		int total = sms_store_list(all, max > 0 ? max : 50);
		struct ojson *o = oj_new();
		int n = total < max ? total : max;

		/* 用一个 raw 数组把消息塞进去 */
		oj_int(o, "total", total);
		oj_int(o, "unread", sms_store_unread());
		{
			char *arr = malloc(64 * 1024);
			size_t used = 0;

			if (!arr) {
				oj_free(o);
				return 1;
			}
			arr[0] = '\0';
			used += (size_t)snprintf(arr + used, 64 * 1024 - used, "[");
			for (int i = 0; i < n; i++) {
				/* 每条的 JSON 单独渲染再拼 —— 避免嵌套 API 复杂化 */
				struct ojson *one = oj_new();
				char *js;

				oj_str(one, "id", all[i].id);
				oj_str(one, "addr", all[i].pdu.addr);
				oj_str(one, "ts", all[i].pdu.ts);
				oj_bool(one, "read", all[i].read);
				oj_bool(one, "outgoing", all[i].outgoing);
				oj_int(one, "received_at", all[i].received_at);
				oj_str(one, "text", all[i].pdu.text);
				js = oj_render(one);
				oj_free(one);
				if (js) {
					used += (size_t)snprintf(arr + used, 64 * 1024 - used,
								 "%s%s", i ? "," : "", js);
					free(js);
				}
			}
			snprintf(arr + used, 64 * 1024 - used, "]");
			oj_raw(o, "messages", arr);
			free(arr);
		}
		oj_emit(o);
		return 0;
	}

	if (!strcmp(act, "read")) {
		struct sms_msg m;

		if (argc < 3)
			return oj_fail("要指定 id");
		if (sms_store_get(argv[2], &m) != 0)
			return oj_fail("找不到这条短信");
		emit_msg_json(&m, 1);
		sms_store_mark(argv[2], 1);
		return 0;
	}

	if (!strcmp(act, "unread")) {
		printf("%d\n", sms_store_unread());
		return 0;
	}

	if (!strcmp(act, "delete")) {
		if (argc < 3)
			return oj_fail("要指定 id 或 all");
		if (!strcmp(argv[2], "all")) {
			int n = sms_store_clear();
			struct ojson *o = oj_new();

			oj_bool(o, "ok", 1);
			oj_int(o, "deleted", n);
			oj_emit(o);
			return 0;
		}
		if (sms_store_delete(argv[2]) != 0)
			return oj_fail("删除失败(没有这条?)");
		struct ojson *o = oj_new();

		oj_bool(o, "ok", 1);
		oj_emit(o);
		return 0;
	}

	if (!strcmp(act, "send")) {
		if (argc < 4)
			return oj_fail("用法: send <号码> <内容>");
		return send_sms(&cfg, argv[2], argv[3]);
	}

	if (!strcmp(act, "selftest")) {
		int bad = 0;
		struct at_port port;

		printf("openfi-sms 自检\n");
		printf("  存储目录: %s(%d 条,未读 %d)\n", sms_store_dir(),
		       sms_store_count(), sms_store_unread());
		if (sms_store_init() != 0) {
			printf("  ❌ 存储目录不可用\n");
			bad++;
		}
		printf("  AT 口: %s @ %d\n", cfg.at_port, cfg.baud);
		if (open_port(&cfg, &port) == 0) {
			struct at_resp r;

			printf("  ✅ 串口可打开\n");
			if (at_cmd(&port, "AT+CPMS?", cfg.timeout_ms, &r) == 0 && r.n > 0)
				printf("  模组存储: %s\n", r.lines[0]);
			at_close(&port);
		} else {
			printf("  ⚠️ 串口打不开(模组没插或口不对)\n");
		}
		printf("\n  %s\n", bad ? "自检发现问题" : "自检通过");
		return bad ? 1 : 0;
	}

	if (!strcmp(act, "collect")) {
		signal(SIGTERM, on_term);
		signal(SIGINT, on_term);
		signal(SIGHUP, on_term);
		olog_info("收短信进程启动,周期 %ds,AT 口 %s", cfg.period, cfg.at_port);
		while (!g_stop) {
			int n = fetch_unread(&cfg);

			if (n > 0)
				olog_info("本轮入库 %d 条", n);
			for (int i = 0; i < cfg.period * 10 && !g_stop; i++)
				usleep(100 * 1000);
		}
		return 0;
	}

	if (!strcmp(act, "-h") || !strcmp(act, "--help")) {
		usage();
		return 0;
	}
	usage();
	return 1;
}
