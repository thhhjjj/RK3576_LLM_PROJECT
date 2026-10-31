/* SPDX-License-Identifier: GPL-2.0 */
#include "metrics.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LLM_CTRL_TEMP_PATH \
	"/sys/devices/platform/llm_ctrl/temp_c"
#define RKNPU_LOAD_PATH "/proc/rknpu/load"
#define NET_DEV_PATH "/proc/net/dev"
#define MEMINFO_PATH "/proc/meminfo"
#define STAT_PATH "/proc/stat"

static char gpu_load_path[PATH_MAX];
static int have_gpu_path;
static char mem_bus_load_path[PATH_MAX];
static int have_mem_bus_path;

static uint64_t prev_idle, prev_total;
static int have_cpu_prev;

static uint64_t prev_rx, prev_tx;
static struct timespec prev_net_ts;
static int have_net_prev;

/* 全部性能项统一 1s 采样；中间调用返回缓存 */
#define METRICS_PERIOD_SEC 1
static struct llm_metrics cached_metrics;
static int have_metrics_cache;
static struct timespec metrics_ts;

static int read_u64_file(const char *path, uint64_t *out)
{
	FILE *f = fopen(path, "r");
	unsigned long long v;

	if (!f)
		return -errno;
	if (fscanf(f, "%llu", &v) != 1) {
		fclose(f);
		return -EINVAL;
	}
	fclose(f);
	*out = (uint64_t)v;
	return 0;
}

static int read_int_file(const char *path, int *out)
{
	FILE *f = fopen(path, "r");
	int v;

	if (!f)
		return -errno;
	if (fscanf(f, "%d", &v) != 1) {
		fclose(f);
		return -EINVAL;
	}
	fclose(f);
	*out = v;
	return 0;
}

/*
 * 探测 GPU load 的 sysfs 路径（用户态只存路径字符串，供之后 open/read）。
 * 内核侧大致：VFS 路径解析 → sysfs（挂在 kernfs 上）→ 驱动导出的 load 属性。
 * 对照：CPU/内存等常走 procfs(/proc)；GPU 调频负载多在 sysfs(/sys/class/devfreq/...)。
 * 错误：返回 -errno
 * 成功：have_gpu_path=1，gpu_load_path="/sys/class/devfreq/<name>/load"
 */
static int probe_gpu_load_path(void)
{
	DIR *d = opendir("/sys/class/devfreq");
	struct dirent *de;
//优先遍历------------------------------------------------------------
	//清空结果路径
	gpu_load_path[0] = '\0';
	have_gpu_path = 0;
	
	//未打开目录，返回-errno
	if (!d)
		return -errno;
	
	//遍历目录
	while ((de = readdir(d)) != NULL) {//读取目录项
		char loadp[PATH_MAX];//加载路径
		const char *n = de->d_name;

		//过滤隐藏文件
		if (n[0] == '.')
			continue;
		//优先匹配mali / gpu
		if (!strstr(n, "gpu") && !strstr(n, "mali") &&
		    !strstr(n, "fb000000") && !strstr(n, "27900000"))
			continue;

		//拼接加载路径
		snprintf(loadp, sizeof(loadp),
			 "/sys/class/devfreq/%s/load", n);
		//判断加载路径是否存在
		if (access(loadp, R_OK) == 0) {
			//复制加载路径到gpu_load_path
			snprintf(gpu_load_path, sizeof(gpu_load_path), "%s",
				 loadp);
			have_gpu_path = 1;//设置加载路径标志
			break;
		}
	}
	closedir(d);
	
//兜底遍历---------------------------------------------------------------
	//未找到加载路径，再次遍历目录
	if (!have_gpu_path) {
		//再次打开目录
		d = opendir("/sys/class/devfreq");
		//未打开目录，返回-ENOENT
		if (!d)
			return -ENOENT;
		//遍历目录
		while ((de = readdir(d)) != NULL) {//读取目录项
			char loadp[PATH_MAX];

			if (de->d_name[0] == '.')
				continue;
			//拼接加载路径
			snprintf(loadp, sizeof(loadp),
				 "/sys/class/devfreq/%s/load", de->d_name);
			//判断加载路径是否存在
			if (access(loadp, R_OK) == 0) {
				//复制加载路径到gpu_load_path
				snprintf(gpu_load_path, sizeof(gpu_load_path),
					 "%s", loadp);
				have_gpu_path = 1;//设置加载路径标志
				break;
			}
		}
		closedir(d);
	}
	return have_gpu_path ? 0 : -ENOENT;
}

/* DDR/DMC 总线占用：Rockchip 通常在 /sys/class/devfreq/<dmc|ddr>/load */
static int probe_mem_bus_load_path(void)
{
	DIR *d = opendir("/sys/class/devfreq");
	struct dirent *de;

	mem_bus_load_path[0] = '\0';
	have_mem_bus_path = 0;
	if (!d)
		return -errno;

	while ((de = readdir(d)) != NULL) {
		char loadp[PATH_MAX];
		const char *n = de->d_name;

		if (n[0] == '.')
			continue;
		if (!strstr(n, "dmc") && !strstr(n, "ddr") &&
		    !strstr(n, "DMC") && !strstr(n, "DDR"))
			continue;
		snprintf(loadp, sizeof(loadp), "/sys/class/devfreq/%s/load", n);
		if (access(loadp, R_OK) == 0) {
			snprintf(mem_bus_load_path, sizeof(mem_bus_load_path),
				 "%s", loadp);
			have_mem_bus_path = 1;
			break;
		}
	}
	closedir(d);
	return have_mem_bus_path ? 0 : -ENOENT;
}

//初始化指标模块
int metrics_init(void)
{
	//清空负载是否有初始值参数
	have_cpu_prev = 0;
	have_net_prev = 0;
	probe_gpu_load_path();
	if (have_gpu_path)
		fprintf(stderr, "metrics: GPU load path %s\n", gpu_load_path);
	else
		fprintf(stderr, "metrics: no GPU load sysfs found\n");
	probe_mem_bus_load_path();
	if (have_mem_bus_path)
		fprintf(stderr, "metrics: mem bus load path %s\n",
			mem_bus_load_path);
	else
		fprintf(stderr, "metrics: mem bus load path not found\n");
	return 0;
}

static int sample_mem(struct llm_metrics *m)
{
	FILE *f = fopen(MEMINFO_PATH, "r");
	char line[256];
	unsigned long long total = 0, avail = 0;
	int got = 0;

	if (!f)
		return -errno;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemTotal: %llu kB", &total) == 1)
			got |= 1;
		else if (sscanf(line, "MemAvailable: %llu kB", &avail) == 1)
			got |= 2;
	}
	fclose(f);
	if (got != 3)
		return -EINVAL;
	m->mem_total_kb = total;
	m->mem_available_kb = avail;
	m->valid |= METRICS_HAVE_MEM;
	return 0;
}

static int metrics_due(void)
{
	struct timespec now;
	double dt;

	if (!have_metrics_cache)
		return 1;
	clock_gettime(CLOCK_MONOTONIC, &now);
	dt = (now.tv_sec - metrics_ts.tv_sec) +
	     (now.tv_nsec - metrics_ts.tv_nsec) / 1e9;
	return dt >= (double)METRICS_PERIOD_SEC;
}

static int sample_cpu(struct llm_metrics *m)
{
	FILE *f = fopen(STAT_PATH, "r");
	char cpu[8];
	uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
	uint64_t idle_all, non_idle, total, didle, dtotal;

	if (!f)
		return -errno;
	if (fscanf(f, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
		   cpu,
		   (unsigned long long *)&user,
		   (unsigned long long *)&nice,
		   (unsigned long long *)&system,
		   (unsigned long long *)&idle,
		   (unsigned long long *)&iowait,
		   (unsigned long long *)&irq,
		   (unsigned long long *)&softirq,
		   (unsigned long long *)&steal) < 5) {
		fclose(f);
		return -EINVAL;
	}
	fclose(f);

	idle_all = idle + iowait;
	non_idle = user + nice + system + irq + softirq + steal;
	total = idle_all + non_idle;

	if (have_cpu_prev) {
		didle = idle_all - prev_idle;
		dtotal = total - prev_total;
		if (dtotal == 0)
			m->cpu_pct = 0;
		else
			m->cpu_pct = (unsigned)((100 * (dtotal - didle)) /
						dtotal);
		m->valid |= METRICS_HAVE_CPU;
	}
	prev_idle = idle_all;
	prev_total = total;
	have_cpu_prev = 1;
	return 0;
}

static int sample_npu_load(struct llm_metrics *m)
{
	FILE *f = fopen(RKNPU_LOAD_PATH, "r");
	char buf[256];
	unsigned a = 0, b = 0;
	int n;

	if (!f)
		return -errno;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -EINVAL;
	}
	fclose(f);

	/* Common forms: "NPU load: 0%" or "0%" or "Core0: 0%, Core1: 0%" */
	n = sscanf(buf, "%*[^0-9]%u%%", &a);
	if (n < 1)
		n = sscanf(buf, "%u%%", &a);
	if (n < 1) {
		/* try average of two cores */
		if (sscanf(buf, "%*[^0-9]%u%*[^0-9]%u", &a, &b) >= 2)
			a = (a + b) / 2;
		else
			return -EINVAL;
	}
	m->npu_load_pct = a;
	m->valid |= METRICS_HAVE_NPU_LOAD;
	return 0;
}

static int sample_npu_temp(struct llm_metrics *m)
{
	int t;

	if (read_int_file(LLM_CTRL_TEMP_PATH, &t) < 0) {
		m->npu_temp_c = INT_MIN;
		return -errno;
	}
	m->npu_temp_c = t;
	m->valid |= METRICS_HAVE_NPU_TEMP;
	return 0;
}

static int sample_gpu(struct llm_metrics *m)
{
	FILE *f;
	unsigned v = 0;
	char buf[64];

	if (!have_gpu_path)
		return -ENOENT;
	f = fopen(gpu_load_path, "r");
	if (!f)
		return -errno;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -EINVAL;
	}
	fclose(f);
	/* "12%" or "12" or "@12%" */
	if (sscanf(buf, "%u%%", &v) != 1 &&
	    sscanf(buf, "@%u%%", &v) != 1 &&
	    sscanf(buf, "%u", &v) != 1)
		return -EINVAL;
	m->gpu_load_pct = v;
	m->valid |= METRICS_HAVE_GPU;
	return 0;
}

static int sample_mem_bus(struct llm_metrics *m)
{
	FILE *f;
	unsigned v = 0;
	char buf[64];

	if (!have_mem_bus_path)
		return -ENOENT;
	f = fopen(mem_bus_load_path, "r");
	if (!f)
		return -errno;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -EINVAL;
	}
	fclose(f);
	if (sscanf(buf, "%u%%", &v) != 1 &&
	    sscanf(buf, "@%u%%", &v) != 1 &&
	    sscanf(buf, "%u", &v) != 1)
		return -EINVAL;
	if (v > 100)
		v = 100;
	m->mem_bus_pct = v;
	m->valid |= METRICS_HAVE_MEM_BUS;
	return 0;
}

static int sample_net(struct llm_metrics *m)
{
	FILE *f = fopen(NET_DEV_PATH, "r");
	char line[512];
	uint64_t rx = 0, tx = 0;
	struct timespec now;
	double dt;
	int found = 0;

	if (!f)
		return -errno;
	/* skip headers */
	if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
		fclose(f);
		return -EINVAL;
	}
	while (fgets(line, sizeof(line), f)) {
		char ifname[32];
		unsigned long long r, t;

		if (sscanf(line,
			   " %31[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
			   ifname, &r, &t) != 3)
			continue;
		if (strcmp(ifname, "wlan0") != 0)
			continue;
		rx = r;
		tx = t;
		found = 1;
		break;
	}
	fclose(f);
	if (!found)
		return -ENOENT;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (have_net_prev) {
		dt = (now.tv_sec - prev_net_ts.tv_sec) +
		     (now.tv_nsec - prev_net_ts.tv_nsec) / 1e9;
		if (dt < 0.05)
			dt = 0.05;
		m->net_rx_Bps = (uint64_t)((rx - prev_rx) / dt);
		m->net_tx_Bps = (uint64_t)((tx - prev_tx) / dt);
		m->valid |= METRICS_HAVE_NET;
	}
	prev_rx = rx;
	prev_tx = tx;
	prev_net_ts = now;
	have_net_prev = 1;
	return 0;
}

int metrics_sample(struct llm_metrics *out)
{
	if (have_metrics_cache && !metrics_due()) {
		*out = cached_metrics;
		return 0;
	}

	memset(out, 0, sizeof(*out));
	out->npu_temp_c = INT_MIN;
	sample_mem(out);
	sample_mem_bus(out);
	sample_cpu(out);
	sample_npu_load(out);
	sample_npu_temp(out);
	sample_gpu(out);
	sample_net(out);

	cached_metrics = *out;
	have_metrics_cache = 1;
	clock_gettime(CLOCK_MONOTONIC, &metrics_ts);
	return 0;
}

void metrics_print(const struct llm_metrics *m)
{
	printf("mem=%llu/%llu kB  mem_bus=%u%%  cpu=%u%%  npu_load=%u%%  "
	       "npu_temp=%s%d%s  gpu=%u%%  rx=%llu B/s  tx=%llu B/s\n",
	       (unsigned long long)m->mem_available_kb,
	       (unsigned long long)m->mem_total_kb,
	       (m->valid & METRICS_HAVE_MEM_BUS) ? m->mem_bus_pct : 0,
	       (m->valid & METRICS_HAVE_CPU) ? m->cpu_pct : 0,
	       (m->valid & METRICS_HAVE_NPU_LOAD) ? m->npu_load_pct : 0,
	       (m->valid & METRICS_HAVE_NPU_TEMP) ? "" : "?",
	       (m->valid & METRICS_HAVE_NPU_TEMP) ? m->npu_temp_c : 0,
	       (m->valid & METRICS_HAVE_NPU_TEMP) ? "C" : "",
	       (m->valid & METRICS_HAVE_GPU) ? m->gpu_load_pct : 0,
	       (unsigned long long)((m->valid & METRICS_HAVE_NET) ?
					   m->net_rx_Bps :
					   0),
	       (unsigned long long)((m->valid & METRICS_HAVE_NET) ?
					   m->net_tx_Bps :
					   0));
	fflush(stdout);
}
