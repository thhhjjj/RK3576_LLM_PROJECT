/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE7-A：/sys/class/devfreq/*npu* 上 performance 升频 / 恢复原 governor。
 */

#include "npu_dvfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NPU_DF_DIR "/sys/class/devfreq"
#define GOV_MAX 64

static char g_base[256];
static char g_gov_saved[GOV_MAX];
static int g_have;
static int g_depth;
static int g_disabled;

static int write_str(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -errno;
	if (fputs(s, f) < 0) {
		int e = -errno;

		fclose(f);
		return e;
	}
	if (fclose(f) != 0)
		return -errno;
	return 0;
}

static int read_str(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");
	size_t len;

	if (!f)
		return -errno;
	if (!fgets(buf, (int)n, f)) {
		fclose(f);
		return -EINVAL;
	}
	fclose(f);
	len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';
	return 0;
}

static void path_join(char *out, size_t n, const char *leaf)
{
	snprintf(out, n, "%s/%s", g_base, leaf);
}

static int probe(void)
{
	DIR *d;
	struct dirent *de;
	char name_path[300];
	char name[128];

	d = opendir(NPU_DF_DIR);
	if (!d)
		return -errno;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(name_path, sizeof(name_path), NPU_DF_DIR "/%s/name",
			 de->d_name);
		name[0] = '\0';
		(void)read_str(name_path, name, sizeof(name));
		if (strstr(de->d_name, "npu") || strstr(name, "npu") ||
		    strstr(name, "rknpu")) {
			snprintf(g_base, sizeof(g_base), NPU_DF_DIR "/%s",
				 de->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);

	snprintf(g_base, sizeof(g_base), NPU_DF_DIR "/27700000.npu");
	if (access(g_base, F_OK) == 0)
		return 0;
	g_base[0] = '\0';
	return -ENOENT;
}

void npu_dvfs_init(void)
{
	const char *env = getenv("LLM_PANEL_NPU_BOOST");
	char gov_path[300];
	char freq_path[300];
	char gov[GOV_MAX];
	char freq[32];

	g_disabled = (env && env[0] == '0');
	g_have = 0;
	g_depth = 0;
	g_gov_saved[0] = '\0';

	if (g_disabled) {
		fprintf(stderr, "npu_dvfs: disabled (LLM_PANEL_NPU_BOOST=0)\n");
		return;
	}
	if (probe() != 0) {
		fprintf(stderr, "npu_dvfs: no NPU devfreq node\n");
		return;
	}
	path_join(gov_path, sizeof(gov_path), "governor");
	path_join(freq_path, sizeof(freq_path), "cur_freq");
	if (read_str(gov_path, gov, sizeof(gov)) != 0) {
		fprintf(stderr, "npu_dvfs: cannot read governor\n");
		return;
	}
	if (read_str(freq_path, freq, sizeof(freq)) != 0)
		snprintf(freq, sizeof(freq), "?");
	g_have = 1;
	fprintf(stderr, "npu_dvfs: %s gov=%s cur=%s\n", g_base, gov, freq);
}

void npu_dvfs_boost(void)
{
	char gov_path[300];
	char freq_path[300];
	char freq[32];

	if (!g_have || g_disabled)
		return;
	if (g_depth++ > 0)
		return;

	path_join(gov_path, sizeof(gov_path), "governor");
	if (read_str(gov_path, g_gov_saved, sizeof(g_gov_saved)) != 0) {
		g_gov_saved[0] = '\0';
		g_depth = 0;
		fprintf(stderr, "npu_dvfs: boost: read governor failed\n");
		return;
	}
	if (write_str(gov_path, "performance") != 0) {
		fprintf(stderr, "npu_dvfs: boost: set performance failed (%s)\n",
			strerror(errno));
		g_depth = 0;
		return;
	}
	path_join(freq_path, sizeof(freq_path), "cur_freq");
	if (read_str(freq_path, freq, sizeof(freq)) == 0)
		fprintf(stderr, "npu_dvfs: boost %s → performance cur=%s\n",
			g_gov_saved, freq);
	else
		fprintf(stderr, "npu_dvfs: boost %s → performance\n",
			g_gov_saved);
}

void npu_dvfs_restore(void)
{
	char gov_path[300];
	char freq_path[300];
	char freq[32];

	if (!g_have || g_disabled)
		return;
	if (g_depth <= 0)
		return;
	if (--g_depth > 0)
		return;
	if (!g_gov_saved[0])
		return;

	path_join(gov_path, sizeof(gov_path), "governor");
	if (write_str(gov_path, g_gov_saved) != 0) {
		fprintf(stderr, "npu_dvfs: restore %s failed\n", g_gov_saved);
		return;
	}
	path_join(freq_path, sizeof(freq_path), "cur_freq");
	if (read_str(freq_path, freq, sizeof(freq)) == 0)
		fprintf(stderr, "npu_dvfs: restore → %s cur=%s\n", g_gov_saved,
			freq);
	else
		fprintf(stderr, "npu_dvfs: restore → %s\n", g_gov_saved);
}
