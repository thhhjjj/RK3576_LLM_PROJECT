/* SPDX-License-Identifier: GPL-2.0 */
/*
 * llm_panel — DRM + LVGL terminal.
 * PHASE2 order: session_load (RKLLM) → then LVGL UI.
 * PHASE3: tokq fan-out → WebSocket :8765.
 */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lv_drivers/display/drm.h"

#include "evdev.h"
#include "font.h"
#include "metrics.h"
#include "cloud_sse.h"
#include "gpu_disp.h"
#include "rkllm_session.h"
#include "tokq.h"
#include "ui.h"
#include "ws_hub.h"

static volatile int g_quit;

/* PHASE2: MemAvailable low-water (MB) → warn + one compress. */
#define MEM_LOW_WATER_MB 200

//退出回调置回调信号
static void on_sig(int sig)
{
	(void)sig;
	g_quit = 1;
}

static void on_ui_compress(void)
{
	if (ui_mode_is_cloud()) {
		ui_append_dialog("\n[云端模式无本地 KV，压缩跳过]\n");
		return;
	}
	ui_append_dialog("\n[压缩中…]\n");
	if (session_compress() != 0)
		ui_append_dialog("[压缩失败]\n");
}

/* 统一提问入口：本地 RKLLM 或云端 DeepSeek（屏 / WS 共用）。 */
static int panel_ask(const char *q)
{
	if (!q || !q[0])
		return -1;
	if (ui_mode_is_cloud()) {
		if (!cloud_sse_has_key())
			return -1;
		if (session_loaded())
			session_abort();
		return cloud_sse_ask(q);
	}
	if (cloud_sse_busy())
		cloud_sse_abort();
	if (!session_loaded())
		return -1;
	return session_ask(q);
}

static void on_ui_ask(void)
{
	const char *q = session_smoke_question();

	if (ui_mode_is_cloud()) {
		if (!cloud_sse_has_key()) {
			ui_append_dialog(
				"\n[云端无 Key：启动前 export DEEPSEEK_API_KEY=… "
				"（仅内存，不落盘）]\n");
			return;
		}
		ui_append_dialog("\n你: ");
		ui_append_dialog(q);
		ui_append_dialog("\n助手(云): ");
		if (panel_ask(q) != 0)
			ui_append_dialog("[云端忙或失败]\n");
		return;
	}
	if (!session_loaded()) {
		ui_append_dialog("\n[无模型：把 librkllmrt.so + QWEN3.5_2B.rkllm 放进镜像]\n");
		return;
	}
	ui_append_dialog("\n你: ");
	ui_append_dialog(q);
	ui_append_dialog("\n助手: ");
	if (panel_ask(q) != 0)
		ui_append_dialog("[忙或失败]\n");
}

static void on_ui_mode(int cloud)
{
	/* 切换时先停掉正在生成的那一路 */
	if (cloud) {
		session_abort();
		ui_append_dialog(cloud_sse_has_key() ?
					 "\n[已切云端 DeepSeek]\n" :
					 "\n[已切云端，但无 API Key]\n");
	} else {
		cloud_sse_abort();
		ui_append_dialog("\n[已切本地]\n");
	}
}

//冲刷打印队列
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

//定时器回调
static void metrics_timer_cb(lv_timer_t *t)
{
	struct llm_metrics m;
	struct session_stats st;
	static int mem_low_latched;

	(void)t;
	drain_tokq();
	{
		char wsbuf[48];

		ws_hub_status(wsbuf, sizeof(wsbuf));
		ui_set_ws_status(wsbuf);
	}
	metrics_sample(&m);
	ui_update_metrics(&m);
	if (gpu_disp_active())
		ui_set_fps(gpu_disp_fps());
	else
		ui_set_fps(-1.f);
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

/* PHASE6：优先 GBM+EGL；失败走 DRM dumb。LLM_PANEL_FORCE_DUMB=1 强制 dumb。 */
static void hal_init(void)
{
	lv_disp_t *disp;
	int gpu = 0;

	lv_init();
	if (gpu_disp_init() == 0) {
		gpu = 1;
		fprintf(stderr, "llm_panel: display=GPU (GBM/EGL)\n");
	} else {
		drm_disp_drv_init(0);
		fprintf(stderr, "llm_panel: display=DRM dumb (CPU)\n");
	}
	gpu_disp_set_refr_60hz();
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
	(void)gpu;
}

int main(int argc, char **argv)
{
	int model_ok;

	(void)argc;
	(void)argv;

	// 向内核注册 SIGINT/SIGTERM 处理函数：收到后设 g_quit，回调on_sig()，
	// 主线程回到 while (!g_quit) 发现该退了就退出
	signal(SIGINT, on_sig);//ctrl c退出
	signal(SIGTERM, on_sig);//kill -pid退出

	/* 会话 / 指标 / token 队列 / 云端 SSE / WebSocket 扇出 */
	session_init();
	metrics_init();
	tokq_init();
	tokq_set_fanout(ws_hub_fanout);
	cloud_sse_init();
	cloud_sse_load_key_from_env(); /* 仅进程内，不落盘 */
	ws_hub_set_ask_handler(panel_ask);

	/* PHASE2：先加载模型，再开 LVGL，避免首屏卡在读权重 */
	printf("llm_panel: loading model (before LVGL)…\n");
	fflush(stdout);

	model_ok = (session_load(NULL) == 0);
	if (!model_ok)
		fprintf(stderr,
			"llm_panel: no RKLLM yet — UI starts without model\n");

	if (ws_hub_start() != 0)
		fprintf(stderr, "llm_panel: WebSocket not started\n");

	hal_init(); /* 显示：优先 GPU，失败回 DRM dumb */
	if (font_init() != 0)
		fprintf(stderr, "llm_panel: using montserrat fallback (no CJK)\n");

	ui_set_actions(on_ui_compress, on_ui_ask);
	ui_set_mode_hook(on_ui_mode);
	ui_create();
	ui_set_model_status(model_ok ? "已加载" : "未加载");
	if (model_ok)
		ui_append_dialog(
			"[PHASE3] 本地可聊；开关切云端=DeepSeek SSE。"
			"Key: DEEPSEEK_API_KEY。WS :8765\n");
	else
		ui_append_dialog(
			"[PHASE3] 无本地模型；可切云端（需 Key）。WS :8765\n");
	if (cloud_sse_has_key())
		ui_append_dialog("[云端 Key 已从环境变量载入（仅内存）]\n");
	else
		ui_append_dialog(
			"[无云端 Key：export DEEPSEEK_API_KEY=… 后重启 panel]\n");

	/* 100ms 刷一次指标 / 抽 tokq / 低水位压缩 */
	lv_timer_create(metrics_timer_cb, 100, NULL);

	printf("llm_panel: running 720x1280 %s (model=%s ws=:%d)\n",
	       gpu_disp_active() ? "GPU" : "DRM-dumb",
	       model_ok ? "yes" : "no", WS_HUB_PORT);
	fflush(stdout);

	while (!g_quit) {
		uint32_t delay;

		/* PHASE7-E（并入 PHASE6）：GPU 路径每圈整屏脏，稳住约 60fps */
		if (gpu_disp_active())
			lv_obj_invalidate(lv_scr_act());

		delay = lv_timer_handler();
		if (gpu_disp_active()) {
			/*
			 * PageFlip 已在垂直同步上阻塞约 16ms。
			 * 这里再 sleep 会把帧率压到约 30–40（板上曾测得 39）。
			 */
			delay = 0;
		} else if (delay > 50) {
			delay = 50; /* dumb 路径：限制空闲休眠上限 */
		}
		if (delay)
			usleep(delay * 1000);
	}

	/* 退出清理 */
	ws_hub_stop();
	cloud_sse_fini();
	session_fini();
	tokq_fini();
	return 0;
}
