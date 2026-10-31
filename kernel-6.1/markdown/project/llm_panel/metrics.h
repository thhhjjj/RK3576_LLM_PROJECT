/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_METRICS_H
#define LLM_PANEL_METRICS_H

#include <stdint.h>

struct llm_metrics {
	uint64_t mem_total_kb;
	uint64_t mem_available_kb;
	unsigned cpu_pct;		/* 0-100, since last sample */
	unsigned npu_load_pct;		/* from /proc/rknpu/load, 0 if missing */
	int npu_temp_c;			/* from llm_ctrl sysfs, INT_MIN if fail */
	unsigned gpu_load_pct;		/* from devfreq load, 0 if missing */
	unsigned mem_bus_pct;		/* DDR/DMC bus load %, 0 if missing */
	uint64_t net_rx_Bps;		/* wlan0 bytes/s since last sample */
	uint64_t net_tx_Bps;
	int valid;			/* bit flags: see METRICS_HAVE_* */
};

#define METRICS_HAVE_MEM	(1u << 0)
#define METRICS_HAVE_CPU	(1u << 1)
#define METRICS_HAVE_NPU_LOAD	(1u << 2)
#define METRICS_HAVE_NPU_TEMP	(1u << 3)
#define METRICS_HAVE_GPU	(1u << 4)
#define METRICS_HAVE_NET	(1u << 5)
#define METRICS_HAVE_MEM_BUS	(1u << 6)

/* Call once at start; discovers GPU load path under /sys/class/devfreq. */
int metrics_init(void);

/* Sample; first call may only fill mem/temp (CPU/net need a prior sample). */
int metrics_sample(struct llm_metrics *out);

/* Print one line to stdout (for serial bring-up, PHASE1 step 32). */
void metrics_print(const struct llm_metrics *m);

#endif
