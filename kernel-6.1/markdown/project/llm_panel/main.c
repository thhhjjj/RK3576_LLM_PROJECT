/* SPDX-License-Identifier: GPL-2.0 */
/*
 * llm_panel — DRM + LVGL terminal.
 * PHASE2 order: session_load (RKLLM) → then LVGL UI.
 */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lv_drivers/display/drm.h"

#include "evdev.h"
#include "font.h"
#include "metrics.h"
#include "rkllm_session.h"
#include "tokq.h"
#include "ui.h"

static volatile int g_quit;

/* PHASE2: MemAvailable low-water (MB) → warn + one compress. */
#define MEM_LOW_WATER_MB 200

static void on_sig(int sig)
{
	(void)sig;
	g_quit = 1;
}

static void on_ui_compress(void)
{
	ui_append_dialog("\n[压缩中…]\n");
	if (session_compress() != 0)
		ui_append_dialog("[压缩失败]\n");
}

static void on_ui_ask(void)
{
	const char *q = session_smoke_question();

	if (!session_loaded()) {
		ui_append_dialog("\n[无模型：把 librkllmrt.so + QWEN3.5_2B.rkllm 放进镜像]\n");
		return;
	}
	ui_append_dialog("\n你: ");
	ui_append_dialog(q);
	ui_append_dialog("\n助手: ");
	if (session_ask(q) != 0)
		ui_append_dialog("[忙或失败]\n");
}

static void drain_tokq(void)
{
	struct tokq_evt ev;
	struct session_stats st;

	while (tokq_pop(&ev)) {
		switch (ev.kind) {
		case TOKQ_TOKEN:
			ui_append_dialog(ev.text);
			break;
		case TOKQ_FINISH:
			ui_append_dialog("\n");
			session_get_stats(&st);
			ui_set_ctx_tps(st.ctx_tokens, st.max_context,
				       st.token_per_s);
			break;
		case TOKQ_ERROR:
			ui_append_dialog("\n[");
			ui_append_dialog(ev.text[0] ? ev.text : "错误");
			ui_append_dialog("]\n");
			break;
		case TOKQ_STATUS:
			if (ev.text[0]) {
				ui_append_dialog("\n");
				ui_append_dialog(ev.text);
				ui_append_dialog("\n");
			}
			session_get_stats(&st);
			ui_set_ctx_tps(st.ctx_tokens, st.max_context,
				       st.token_per_s);
			break;
		}
	}
}

static void metrics_timer_cb(lv_timer_t *t)
{
	struct llm_metrics m;
	struct session_stats st;
	static int mem_low_latched;

	(void)t;
	drain_tokq();
	metrics_sample(&m);
	ui_update_metrics(&m);
	session_get_stats(&st);
	if (st.loaded)
		ui_set_ctx_tps(st.ctx_tokens, st.max_context, st.token_per_s);

	if ((m.valid & METRICS_HAVE_MEM) && session_loaded()) {
		unsigned long long avail_mb = m.mem_available_kb / 1024;

		if (avail_mb < MEM_LOW_WATER_MB) {
			if (!mem_low_latched) {
				char buf[96];

				mem_low_latched = 1;
				snprintf(buf, sizeof(buf),
					 "\n[内存低水位 %llu MB < %d MB，触发压缩]\n",
					 avail_mb, MEM_LOW_WATER_MB);
				ui_append_dialog(buf);
				if (session_compress() != 0)
					ui_append_dialog("[低水位压缩失败]\n");
			}
		} else if (avail_mb > MEM_LOW_WATER_MB + 50) {
			/* Hysteresis: clear latch when recovered. */
			mem_low_latched = 0;
		}
	}
}

static void hal_init(void)
{
	lv_disp_t *disp;

	lv_init();
	drm_disp_drv_init(0);
	//获取默认显示器
	disp = lv_disp_get_default();
	//如果显示器存在且初始化成功，则初始化输入设备
	if (disp && evdev_init(disp->driver, 0) == 0) {
		//初始化输入设备驱动
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
	int model_ok;

	(void)argc;
	(void)argv;

	//注册声明退出信号
	signal(SIGINT, on_sig);//ctrl c退出
	signal(SIGTERM, on_sig);//kill -pid退出
	//初始化会话模块
	session_init();
	//sysfs 里找到 GPU 的 load 节点路径记录
	metrics_init();

	/* Hard order: load RKLLM before LVGL widgets. */
	printf("llm_panel: loading model (before LVGL)…\n");
	//冲刷打印
	fflush(stdout);
	//加载模型
	model_ok = (session_load(NULL) == 0);
	//如果模型加载失败，打印错误信息
	if (!model_ok)
		fprintf(stderr,
			"llm_panel: no RKLLM yet — UI starts without model\n");

	hal_init();//初始化硬件抽象层
	if (font_init() != 0)//FreeType 加载思源黑体
		fprintf(stderr, "llm_panel: using montserrat fallback (no CJK)\n");

	ui_set_actions(on_ui_compress, on_ui_ask);//设置UI操作回调函数
	ui_create();//创建UI控件
	ui_set_model_status(model_ok ? "已加载" : "未加载");
	if (model_ok)
		ui_append_dialog("[PHASE2] 模型已在 UI 前加载。点「试问」开始。\n");
	else
		ui_append_dialog(
			"[PHASE2] 未找到 /usr/lib/librkllmrt.so 或 "
			"/usr/share/rkllm/QWEN3.5_2B.rkllm\n"
			"见 makediff/rkllm_autoload/README.md\n");

	lv_timer_create(metrics_timer_cb, 100, NULL);//创建100ms的定时器回调

	printf("llm_panel: running 720x1280 DRM (model=%s)\n",
	       model_ok ? "yes" : "no");
	fflush(stdout);

	while (!g_quit) {
		uint32_t delay = lv_timer_handler();//50ms

		if (delay > 50)
			delay = 50;
		usleep(delay * 1000);
	}

	session_fini();
	return 0;
}
