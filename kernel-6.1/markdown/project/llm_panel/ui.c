/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE1 LVGL panel: 6 metrics + dialog + compress + local/cloud.
 * Clicks only toggle UI state — no model load, no network.
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
static lv_obj_t *lbl_net;
static lv_obj_t *lbl_ctx;
static lv_obj_t *lbl_tps;
static lv_obj_t *lbl_overtemp;
static lv_obj_t *btn_compress;
static lv_obj_t *sw_mode;
static lv_obj_t *lbl_mode;
static int compress_on;
static int mode_cloud; /* 0 local, 1 cloud */

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
	lv_obj_t *btn = lv_event_get_target(e);
	compress_on = !compress_on;
	lv_obj_t *lab = lv_obj_get_child(btn, 0);
	if (lab)
		lv_label_set_text(lab, compress_on ? "压缩:开" : "压缩:关");
}

static void on_mode(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target(e);
	mode_cloud = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
	if (lbl_mode)
		lv_label_set_text(lbl_mode, mode_cloud ? "云端" : "本地");
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

void ui_create(void)
{
	lv_obj_t *scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "LLM 终端");
	style_text(title, font_title(), 0xe8e8e8);
	lv_obj_set_pos(title, 16, 12);

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

	lv_obj_t *dlg = lv_textarea_create(scr);
	lv_obj_set_size(dlg, 688, 720);
	lv_obj_set_pos(dlg, 16, 260);
	lv_obj_set_style_text_font(dlg, font_body(), 0);
	lv_textarea_set_text(dlg,
		"（对话区）\nPHASE1：点击只改变按钮状态，不加载模型。");
	lv_textarea_set_cursor_click_pos(dlg, false);
	lv_obj_clear_flag(dlg, LV_OBJ_FLAG_CLICK_FOCUSABLE);

	btn_compress = lv_btn_create(scr);
	lv_obj_set_size(btn_compress, 200, 56);
	lv_obj_set_pos(btn_compress, 16, 1020);
	lv_obj_add_event_cb(btn_compress, on_compress, LV_EVENT_CLICKED, NULL);
	{
		lv_obj_t *lab = lv_label_create(btn_compress);
		lv_label_set_text(lab, "压缩:关");
		lv_obj_set_style_text_font(lab, font_body(), 0);
		lv_obj_center(lab);
	}

	sw_mode = lv_switch_create(scr);
	lv_obj_set_pos(sw_mode, 240, 1030);
	lv_obj_add_event_cb(sw_mode, on_mode, LV_EVENT_VALUE_CHANGED, NULL);

	lbl_mode = lv_label_create(scr);
	lv_label_set_text(lbl_mode, "本地");
	style_text(lbl_mode, font_body(), 0xe8e8e8);
	lv_obj_set_pos(lbl_mode, 320, 1036);

	lbl_overtemp = lv_label_create(scr);
	lv_label_set_text(lbl_overtemp, "");
	style_text(lbl_overtemp, font_body(), 0xff5555);
	lv_obj_set_pos(lbl_overtemp, 16, 1088);
	lv_obj_add_flag(lbl_overtemp, LV_OBJ_FLAG_HIDDEN);
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
