/* SPDX-License-Identifier: MIT */
#include "frpc_ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int conf_looks_ready(void)
{
	FILE *fp;
	char line[256];
	int has_server = 0, has_placeholder = 0;

	fp = fopen(FRPC_CONF, "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "serverAddr") && strstr(line, "CHANGE_ME"))
			has_placeholder = 1;
		if (strstr(line, "serverAddr") && !strstr(line, "CHANGE_ME") &&
		    strchr(line, '"'))
			has_server = 1;
		if (strstr(line, "auth.token") && strstr(line, "CHANGE_ME"))
			has_placeholder = 1;
	}
	fclose(fp);
	if (has_placeholder)
		return 0;
	return has_server;
}

static pid_t read_pidfile(void)
{
	FILE *fp;
	long p = 0;

	fp = fopen(FRPC_PIDFILE, "r");
	if (!fp)
		return 0;
	if (fscanf(fp, "%ld", &p) != 1)
		p = 0;
	fclose(fp);
	return (pid_t)p;
}

static int pid_alive(pid_t pid)
{
	if (pid <= 1)
		return 0;
	return kill(pid, 0) == 0 || errno == EPERM;
}

int frpc_ctl_running(void)
{
	pid_t pid = read_pidfile();

	if (pid_alive(pid))
		return 1;
	/* 兜底：按进程名 */
	if (system("pidof frpc >/dev/null 2>&1") == 0)
		return 1;
	return 0;
}

int frpc_ctl_stop(void)
{
	pid_t pid = read_pidfile();

	if (pid_alive(pid)) {
		kill(pid, SIGTERM);
		usleep(300000);
		if (pid_alive(pid))
			kill(pid, SIGKILL);
	}
	system("killall -q frpc 2>/dev/null");
	unlink(FRPC_PIDFILE);
	return 0;
}

int frpc_ctl_start(void)
{
	pid_t pid;
	int fd;
	struct stat st;

	if (frpc_ctl_running())
		return 0;
	if (stat(FRPC_BIN, &st) != 0 || !(st.st_mode & S_IXUSR)) {
		fprintf(stderr, "frpc_ctl: missing %s\n", FRPC_BIN);
		return -1;
	}
	if (!conf_looks_ready()) {
		fprintf(stderr, "frpc_ctl: edit %s (serverAddr/token)\n",
			FRPC_CONF);
		return -1;
	}

	frpc_ctl_stop();

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int logfd = open(FRPC_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (logfd >= 0) {
			dup2(logfd, STDOUT_FILENO);
			dup2(logfd, STDERR_FILENO);
			close(logfd);
		}
		setsid();
		execl(FRPC_BIN, "frpc", "-c", FRPC_CONF, (char *)NULL);
		_exit(127);
	}

	fd = open(FRPC_PIDFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		char buf[32];
		int n = snprintf(buf, sizeof(buf), "%ld\n", (long)pid);
		if (n > 0)
			(void)write(fd, buf, (size_t)n);
		close(fd);
	}
	usleep(200000);
	if (!pid_alive(pid)) {
		fprintf(stderr, "frpc_ctl: exited early, see %s\n", FRPC_LOG);
		unlink(FRPC_PIDFILE);
		return -1;
	}
	fprintf(stderr, "frpc_ctl: started pid=%ld\n", (long)pid);
	return 0;
}

void frpc_ctl_status(char *buf, int buflen)
{
	struct stat st;

	if (!buf || buflen < 8)
		return;
	if (stat(FRPC_BIN, &st) != 0) {
		snprintf(buf, buflen, "frpc 缺二进制");
		return;
	}
	if (!conf_looks_ready()) {
		snprintf(buf, buflen, "frpc 缺配置");
		return;
	}
	if (frpc_ctl_running())
		snprintf(buf, buflen, "frpc 开");
	else
		snprintf(buf, buflen, "frpc 关");
}
