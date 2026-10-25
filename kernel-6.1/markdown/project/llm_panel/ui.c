/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE1–3 LVGL panel: metrics + dialog + compress + ask + local/cloud.
 * PHASE3: mode hook wired; cloud SSE next. WS status label.
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "font.h"
#include "lvgl/lvgl.h"

static lv_obj_t *lbl_mem;
static lv_obj_t *lbl_cpu;
static lv_obj_t *lbl_npu_load;
static lv_obj_t *lbl_npu_temp;
static lv_obj_t *lbl_gpu;
static lv_obj_t *lbl_fps;
static lv_obj_t *lbl_net;
static lv_obj_t *lbl_ctx;
static lv_obj_t *lbl_tps;
static lv_obj_t *lbl_overtemp;
static lv_obj_t *lbl_model;
static lv_obj_t *ta_dialog;
static lv_obj_t *btn_compress;
static lv_obj_t *btn_ask;
static lv_obj_t *sw_mode;
static lv_obj_t *lbl_mode;
static lv_obj_t *lbl_ws;
static int mode_cloud; /* 0 local, 1 cloud */

static ui_action_fn act_compress;
static ui_action_fn act_ask;
static ui_mode_fn act_mode;

static void set_label(lv_obj_t *lbl, const char *txt)
{
	if (lbl)
		lv_label_set_text(lbl, txt);
}

static void style_text(lv_obj_t *obj, const lv_font_t *font, uint32_t color)
{
	lv_obj_set_style_text_font(obj, font, 0);
	lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

static void on_compress(lv_event_t *e)
{
	(void)e;
	if (act_compress)
		act_compress();
}

static void on_ask(lv_event_t *e)
{
	(void)e;
	if (act_ask)
		act_ask();
}

static void on_mode(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target(e);

	mode_cloud = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
	if (lbl_mode)
		lv_label_set_text(lbl_mode,
				  mode_cloud ? "云端" : "本地");
	if (act_mode)
		act_mode(mode_cloud);
}

static lv_obj_t *add_metric_row(lv_obj_t *parent, const char *title, int y)
{
	lv_obj_t *t = lv_label_create(parent);
	lv_label_set_text(t, title);
	style_text(t, font_small(), 0xffffff);
	lv_obj_set_pos(t, 16, y);

	lv_obj_t *v = lv_label_create(parent);
	lv_label_set_text(v, "—");
	style_text(v, font_body(), 0xffffff);
	lv_obj_set_pos(v, 200, y);
	return v;
}

void ui_set_actions(ui_action_fn on_compress_fn, ui_action_fn on_ask_fn)
{
	act_compress = on_compress_fn;
	act_ask = on_ask_fn;
}

void ui_set_mode_hook(ui_mode_fn on_mode_fn)
{
	act_mode = on_mode_fn;
}

int ui_mode_is_cloud(void)
{
	return mode_cloud;
}

void ui_create(void)
{
	lv_obj_t *scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "LLM 终端");
	style_text(title, font_title(), 0xe8e8e8);
	lv_obj_set_pos(title, 16, 12);

	lbl_model = lv_label_create(scr);
	lv_label_set_text(lbl_model, "模型: —");
	style_text(lbl_model, font_small(), 0xa0a8b0);
	lv_obj_set_pos(lbl_model, 200, 16);

	lbl_mem = add_metric_row(scr, "内存", 48);
	lbl_cpu = add_metric_row(scr, "CPU", 72);
	lbl_npu_load = add_metric_row(scr, "NPU 负载", 96);
	lbl_npu_temp = add_metric_row(scr, "NPU 温度", 120);
	lbl_gpu = add_metric_row(scr, "GPU", 144);
	lbl_net = add_metric_row(scr, "上行/下行", 168);

	lbl_ctx = add_metric_row(scr, "上下文", 200);
	lbl_tps = add_metric_row(scr, "token/s", 224);
	set_label(lbl_ctx, "—");
	set_label(lbl_tps, "—");

	ta_dialog = lv_textarea_create(scr);
	lv_obj_set_size(ta_dialog, 688, 680);
	lv_obj_set_pos(ta_dialog, 16, 260);
	lv_obj_set_style_text_font(ta_dialog, font_body(), 0);
	lv_textarea_set_text(ta_dialog,
			     "（对话区）\nPHASE3：本地 / 云端 DeepSeek；WS :8765。\n");
	lv_textarea_set_cursor_click_pos(ta_dialog, false);
	lv_obj_clear_flag(ta_dialog, LV_OBJ_FLAG_CLICK_FOCUSABLE);

	btn_compress = lv_btn_create(scr);
	lv_obj_set_size(btn_compress, 160, 56);
	lv_obj_set_pos(btn_compress, 16, 980);
	lv_obj_add_event_cb(btn_compress, on_compress, LV_EVENT_CLICKED, NULL);
	{
		lv_obj_t *lab = lv_label_create(btn_compress);
		lv_label_set_text(lab, "压缩");
		lv_obj_set_style_text_font(lab, font_body(), 0);
		lv_obj_center(lab);
	}

	btn_ask = lv_btn_create(scr);
	lv_obj_set_size(btn_ask, 160, 56);
	lv_obj_set_pos(btn_ask, 200, 980);
	lv_obj_add_event_cb(btn_ask, on_ask, LV_EVENT_CLICKED, NULL);
	{
		lv_obj_t *lab = lv_label_create(btn_ask);
		lv_label_set_text(lab, "试问");
		lv_obj_set_style_text_font(lab, font_body(), 0);
		lv_obj_center(lab);
	}

	sw_mode = lv_switch_create(scr);
	lv_obj_set_pos(sw_mode, 400, 990);
	lv_obj_add_event_cb(sw_mode, on_mode, LV_EVENT_VALUE_CHANGED, NULL);

	lbl_mode = lv_label_create(scr);
	lv_label_set_text(lbl_mode, "本地");
	style_text(lbl_mode, font_body(), 0xe8e8e8);
	lv_obj_set_pos(lbl_mode, 480, 996);

	/* PHASE7-E: fps top-right; WS just under it */
	lbl_fps = lv_label_create(scr);
	lv_label_set_text(lbl_fps, "— fps");
	style_text(lbl_fps, font_body(), 0xffffff);
	lv_obj_align(lbl_fps, LV_ALIGN_TOP_RIGHT, -16, 12);

	lbl_ws = lv_label_create(scr);
	lv_label_set_text(lbl_ws, "WS —");
	style_text(lbl_ws, font_small(), 0xa0a8b0);
	lv_obj_align(lbl_ws, LV_ALIGN_TOP_RIGHT, -16, 44);

	lbl_overtemp = lv_label_create(scr);
	lv_label_set_text(lbl_overtemp, "");
	style_text(lbl_overtemp, font_body(), 0xff5555);
	lv_obj_set_pos(lbl_overtemp, 16, 1088);
	lv_obj_add_flag(lbl_overtemp, LV_OBJ_FLAG_HIDDEN);

	/* Build/flash marker — bottom-right, white. */
	{
		lv_obj_t *ver = lv_label_create(scr);
		lv_label_set_text(ver, "vtest：0.6.0");
		style_text(ver, font_small(), 0xffffff);
		lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
	}
}

void ui_update_metrics(const struct llm_metrics *m)
{
	char buf[128];

	if (m->valid & METRICS_HAVE_MEM) {
		unsigned long long used =
			m->mem_total_kb > m->mem_available_kb ?
				m->mem_total_kb - m->mem_available_kb :
				0;
		snprintf(buf, sizeof(buf), "%llu / %llu MB",
			 used / 1024, m->mem_total_kb / 1024);
		set_label(lbl_mem, buf);
	}
	if (m->valid & METRICS_HAVE_CPU) {
		snprintf(buf, sizeof(buf), "%u%%", m->cpu_pct);
		set_label(lbl_cpu, buf);
	}
	if (m->valid & METRICS_HAVE_NPU_LOAD) {
		snprintf(buf, sizeof(buf), "%u%%", m->npu_load_pct);
		set_label(lbl_npu_load, buf);
	}
	if (m->valid & METRICS_HAVE_NPU_TEMP) {
		snprintf(buf, sizeof(buf), "%d ℃", m->npu_temp_c);
		set_label(lbl_npu_temp, buf);
	}
	if (m->valid & METRICS_HAVE_GPU) {
		snprintf(buf, sizeof(buf), "%u%%", m->gpu_load_pct);
		set_label(lbl_gpu, buf);
	}
	if (m->valid & METRICS_HAVE_NET) {
		snprintf(buf, sizeof(buf), "%llu / %llu B/s",
			 (unsigned long long)m->net_tx_Bps,
			 (unsigned long long)m->net_rx_Bps);
		set_label(lbl_net, buf);
	}
}

void ui_show_overtemp(int temp_c)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "超温 %d ℃", temp_c);
	set_label(lbl_overtemp, buf);
	lv_obj_clear_flag(lbl_overtemp, LV_OBJ_FLAG_HIDDEN);
}

void ui_clear_overtemp(void)
{
	lv_obj_add_flag(lbl_overtemp, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_dialog(const char *text)
{
	if (ta_dialog && text)
		lv_textarea_set_text(ta_dialog, text);
}

void ui_append_dialog(const char *text)
{
	if (ta_dialog && text && text[0])
		lv_textarea_add_text(ta_dialog, text);
}

void ui_set_ctx_tps(int ctx, int max_ctx, float tps)
{
	char buf[64];

	if (ctx < 0)
		set_label(lbl_ctx, "—");
	else {
		snprintf(buf, sizeof(buf), "%d / %d", ctx, max_ctx);
		set_label(lbl_ctx, buf);
	}
	if (tps <= 0.01f)
		set_label(lbl_tps, "—");
	else {
		snprintf(buf, sizeof(buf), "%.1f", tps);
		set_label(lbl_tps, buf);
	}
}

void ui_set_model_status(const char *text)
{
	char buf[96];

	snprintf(buf, sizeof(buf), "模型: %s", text ? text : "—");
	set_label(lbl_model, buf);
}

void ui_set_ws_status(const char *text)
{
	set_label(lbl_ws, text ? text : "WS —");
}

void ui_set_fps(float fps)
{
	char buf[32];

	if (fps < 0.f) {
		set_label(lbl_fps, "— fps");
		return;
	}
	snprintf(buf, sizeof(buf), "%.0f fps", fps);
	set_label(lbl_fps, buf);
	if (lbl_fps)
		lv_obj_align(lbl_fps, LV_ALIGN_TOP_RIGHT, -16, 12);
}
