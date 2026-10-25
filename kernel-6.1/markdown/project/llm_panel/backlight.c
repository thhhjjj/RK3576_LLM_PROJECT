/* SPDX-License-Identifier: GPL-2.0 */
#include "backlight.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_path[256];
static int g_max = 255;
static int g_pct = 80;

static int write_int(const char *path, int v)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;
	fprintf(f, "%d", v);
	fclose(f);
	return 0;
}

static int read_int(const char *path, int *out)
{
	FILE *f = fopen(path, "r");
	int v;

	if (!f)
		return -1;
	if (fscanf(f, "%d", &v) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*out = v;
	return 0;
}

void backlight_init(void)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *de;
	char maxp[300], curp[300];
	int cur = 0;

	g_path[0] = '\0';
	if (!d) {
		fprintf(stderr, "backlight: no /sys/class/backlight\n");
		return;
	}
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(g_path, sizeof(g_path),
			 "/sys/class/backlight/%s/brightness", de->d_name);
		snprintf(maxp, sizeof(maxp),
			 "/sys/class/backlight/%s/max_brightness", de->d_name);
		if (read_int(maxp, &g_max) == 0 && g_max > 0)
			break;
		g_path[0] = '\0';
	}
	closedir(d);
	if (!g_path[0]) {
		fprintf(stderr, "backlight: no device\n");
		return;
	}
	snprintf(curp, sizeof(curp), "%s", g_path);
	if (read_int(curp, &cur) == 0 && g_max > 0)
		g_pct = (cur * 100 + g_max / 2) / g_max;
	if (g_pct < 20)
		g_pct = 20;
	if (g_pct > 100)
		g_pct = 100;
	fprintf(stderr, "backlight: %s max=%d pct=%d\n", g_path, g_max, g_pct);
}

int backlight_get_pct(void)
{
	return g_pct;
}

int backlight_set_pct(int pct)
{
	int v;

	if (pct < 20)
		pct = 20;
	if (pct > 100)
		pct = 100;
	g_pct = pct;
	if (!g_path[0] || g_max <= 0)
		return -1;
	v = (pct * g_max + 50) / 100;
	if (v < 1)
		v = 1;
	if (v > g_max)
		v = g_max;
	return write_int(g_path, v);
}
