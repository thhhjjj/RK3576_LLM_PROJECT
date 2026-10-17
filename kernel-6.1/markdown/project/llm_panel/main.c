/* SPDX-License-Identifier: GPL-2.0 */
/*
 * llm_panel — PHASE1 DRM + LVGL terminal shell (no model / no sockets).
 *
 * Built by Buildroot package llm_panel against staging lvgl + lv_drivers.
 */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lv_drivers/display/drm.h"

#include "evdev.h"
#include "metrics.h"
#include "ui.h"

static volatile int g_quit;

static void on_sig(int sig)
{
	(void)sig;
	g_quit = 1;
}

static void metrics_timer_cb(lv_timer_t *t)
{
	struct llm_metrics m;

	(void)t;
	metrics_sample(&m);
	ui_update_metrics(&m);
}

static void hal_init(void)
{
	lv_disp_t *disp;

	lv_init();

	/* Portrait MIPI 720x1280; rotation 0 for PHASE1 portrait. */
	drm_disp_drv_init(0);

	disp = lv_disp_get_default();
	if (disp && evdev_init(disp->driver, 0) == 0) {
		static lv_indev_drv_t indev_drv;

		lv_indev_drv_init(&indev_drv);
		indev_drv.type = LV_INDEV_TYPE_POINTER;
		indev_drv.read_cb = evdev_read;
		lv_indev_drv_register(&indev_drv);
	} else {
		fprintf(stderr, "llm_panel: evdev init failed (UI still runs)\n");
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	metrics_init();
	hal_init();
	ui_create();
	lv_timer_create(metrics_timer_cb, 1000, NULL);

	printf("llm_panel: running 720x1280 DRM\n");
	fflush(stdout);

	while (!g_quit) {
		uint32_t delay = lv_timer_handler();
		if (delay > 50)
			delay = 50;
		usleep(delay * 1000);
	}
	return 0;
}
