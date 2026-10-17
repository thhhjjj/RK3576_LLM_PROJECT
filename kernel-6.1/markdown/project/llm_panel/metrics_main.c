/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE1 step 32: print metrics to serial / adb before binding LVGL.
 *
 * Cross-build on Ubuntu:
 *   aarch64-linux-gnu-gcc -Wall -O2 -static -o metrics_test \
 *       metrics_main.c metrics.c
 *   adb push metrics_test /tmp/ && adb shell chmod +x /tmp/metrics_test
 *   adb shell /tmp/metrics_test
 */

#include "metrics.h"

#include <stdio.h>
#include <unistd.h>

int main(void)
{
	struct llm_metrics m;
	int i;

	metrics_init();
	for (i = 0; i < 8; i++) {
		metrics_sample(&m);
		metrics_print(&m);
		sleep(1);
	}
	return 0;
}
