/*
 * 假 5G 模组 —— 用 PTY 模拟一个真的移远 RM500U-CN
 *
 * 它按真机的报文格式应答(含命令回显、URC 干扰行、多行温度),
 * 于是 openfi-modem 的【完整流程】—— 找串口、发命令、解析、写缓存 ——
 * 都能在开发机上跑一遍,不用把模组接上。
 *
 * 用法: fake-modem [-p <已有 pty 从端>] [-v]
 *   不带 -p 时自己开一个 pty,把从端设备名打到 stdout(第一行)后驻留。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <pty.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int verbose;

/* 真机报文(照抄抓包) */
static const char *resp_for(const char *cmd)
{
	if (!strcmp(cmd, "AT"))            return "OK\r\n";
	if (!strcmp(cmd, "AT+CPIN?"))      return "+CPIN: READY\r\n\r\nOK\r\n";
	if (!strcmp(cmd, "AT+CGSN"))       return "+CGSN: 012345678901234\r\n\r\nOK\r\n";
	if (!strcmp(cmd, "AT+CIMI"))       return "460001234567890\r\nOK\r\n";
	if (!strcmp(cmd, "AT+QCCID"))      return "+QCCID: 89860112345678901234\r\n\r\nOK\r\n";
	if (!strcmp(cmd, "AT+CGMM"))       return "RM500U-CN\r\nOK\r\n";
	if (!strcmp(cmd, "AT+CGMR"))       return "RM500UCNCBR01A03M4G\r\nOK\r\n";
	if (!strcmp(cmd, "AT+CSQ"))        return "+CSQ: 21,98\r\n\r\nOK\r\n";
	if (!strcmp(cmd, "AT+COPS?"))      return "+COPS: 0,0,\"CHINA MOBILE\",7\r\n\r\nOK\r\n";
	if (!strcmp(cmd, "AT+CEREG?"))     return "+CEREG: 0,1,\"1234\",\"0A1B2C3\",7\r\n\r\nOK\r\n";
	if (!strcmp(cmd, "AT+CREG?"))      return "+CREG: 0,1\r\n\r\nOK\r\n";
	if (!strcmp(cmd, "AT+QENG=\"servingcell\""))
		return "+QENG: \"servingcell\",\"NOCONN\",\"NR5G-SA\",\"TDD\",460,00,"
		       "123456789,266,504990,41,5,-81,-10,-73,16,-\r\n\r\nOK\r\n";
	if (!strcmp(cmd, "AT+QTEMP"))
		return "+QTEMP: \"soc-thermal\",54\r\n"
		       "+QTEMP: \"pa-thermal\",47\r\nOK\r\n";
	if (!strcmp(cmd, "AT+QNWINFO"))
		return "+QNWINFO: \"NR5G-SA\",\"46000\",\"41\",\"504990\"\r\n\r\nOK\r\n";
	/* 不认识的命令:真模组回 ERROR 或 CME ERROR */
	return "ERROR\r\n";
}

int main(int argc, char **argv)
{
	int master = -1, slave = -1;
	char slave_name[128] = "";
	int opt;
	FILE *out;

	while ((opt = getopt(argc, argv, "p:v")) != -1) {
		switch (opt) {
		case 'p':
			snprintf(slave_name, sizeof(slave_name), "%s", optarg);
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "用法: fake-modem [-p <pty从端>] [-v]\n");
			return 1;
		}
	}

	if (!slave_name[0]) {
		if (openpty(&master, &slave, slave_name, NULL, NULL) != 0) {
			perror("openpty");
			return 1;
		}
		/*
		 * ★ 千万别 close(slave)。
		 *   devpts 的节点(/dev/pts/N)是靠"至少有一个已打开的 fd"维持的 ——
		 *   关掉最后一个从端 fd,那个节点就消失了,调用方再 open 会得到
		 *   ENOENT(我第一次就是这么写的,测试立刻报"拿不到 pty")。
		 *   所以这里【保留】从端 fd,让节点一直存在;调用方再去开同一个节点,
		 *   两边共享从端 —— 这也是 socat 那类工具的做法。
		 */
	}

	/* 串口参数:raw,不要回显加工(我们自己造回显)*/
	{
		int fd = master >= 0 ? master : open(slave_name, O_RDWR | O_NOCTTY);

		if (fd < 0) {
			perror(slave_name);
			return 1;
		}
		master = fd;
	}

	/* 第一行打印从端设备名,方便脚本抓 */
	out = fdopen(dup(STDOUT_FILENO), "w");
	if (out) {
		fprintf(out, "%s\n", slave_name);
		fflush(out);
		fclose(out);
	}

	/* 主循环:按行读命令,回显 + 应答 */
	char line[512];
	size_t used = 0;

	for (;;) {
		char c;
		ssize_t n = read(master, &c, 1);

		if (n <= 0)
			break;
		if (c == '\r') {
			line[used] = '\0';
			used = 0;
			if (verbose)
				fprintf(stderr, "[fake-modem] 收到: %s\n", line);
			if (!line[0])
				continue;
			/* 真机开着回显:先原样回一行 */
			dprintf(master, "%s\r\r\n", line);
			dprintf(master, "%s", resp_for(line));
			continue;
		}
		if (c == '\n')
			continue;
		if (used < sizeof(line) - 1)
			line[used++] = c;
	}
	close(master);
	return 0;
}
