/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE1 LVGL panel: 6 metrics + dialog + compress + local/cloud.
 * Clicks only toggle UI state — no model load, no network.
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>

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

static void on_compress(lv_event_t *e)
{
	lv_obj_t *btn = lv_event_get_target(e);
	compress_on = !compress_on;
	lv_obj_t *lab = lv_obj_get_child(btn, 0);
	if (lab)
		lv_label_set_text(lab, compress_on ? "Compress:ON" : "Compress:OFF");
}

static void on_mode(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target(e);
	mode_cloud = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
	if (lbl_mode)
		lv_label_set_text(lbl_mode, mode_cloud ? "Cloud" : "Local");
}

static lv_obj_t *add_metric_row(lv_obj_t *parent, const char *title, int y)
{
	lv_obj_t *t = lv_label_create(parent);
	lv_label_set_text(t, title);
	lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
	lv_obj_set_pos(t, 16, y);

	lv_obj_t *v = lv_label_create(parent);
	lv_label_set_text(v, "—");
	lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
	lv_obj_set_pos(v, 200, y);
	return v;
}

void ui_create(void)
{
	lv_obj_t *scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "LLM Terminal");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(0xe8e8e8), 0);
	lv_obj_set_pos(title, 16, 12);

	lbl_mem = add_metric_row(scr, "Mem", 48);
	lbl_cpu = add_metric_row(scr, "CPU", 72);
	lbl_npu_load = add_metric_row(scr, "NPU load", 96);
	lbl_npu_temp = add_metric_row(scr, "NPU temp", 120);
	lbl_gpu = add_metric_row(scr, "GPU", 144);
	lbl_net = add_metric_row(scr, "Net up/dn", 168);

	lbl_ctx = add_metric_row(scr, "Context", 200);
	lbl_tps = add_metric_row(scr, "token/s", 224);
	set_label(lbl_ctx, "—");
	set_label(lbl_tps, "—");

	lv_obj_t *dlg = lv_textarea_create(scr);
	lv_obj_set_size(dlg, 688, 720);
	lv_obj_set_pos(dlg, 16, 260);
	lv_textarea_set_text(dlg, "(dialog)\nPHASE1: tap only changes button state.");
	lv_textarea_set_cursor_click_pos(dlg, false);
	lv_obj_clear_flag(dlg, LV_OBJ_FLAG_CLICK_FOCUSABLE);

	btn_compress = lv_btn_create(scr);
	lv_obj_set_size(btn_compress, 200, 56);
	lv_obj_set_pos(btn_compress, 16, 1020);
	lv_obj_add_event_cb(btn_compress, on_compress, LV_EVENT_CLICKED, NULL);
	{
		lv_obj_t *lab = lv_label_create(btn_compress);
		lv_label_set_text(lab, "Compress:OFF");
		lv_obj_center(lab);
	}

	sw_mode = lv_switch_create(scr);
	lv_obj_set_pos(sw_mode, 240, 1030);
	lv_obj_add_event_cb(sw_mode, on_mode, LV_EVENT_VALUE_CHANGED, NULL);

	lbl_mode = lv_label_create(scr);
	lv_label_set_text(lbl_mode, "Local");
	lv_obj_set_pos(lbl_mode, 320, 1036);

	lbl_overtemp = lv_label_create(scr);
	lv_label_set_text(lbl_overtemp, "");
	lv_obj_set_style_text_color(lbl_overtemp, lv_color_hex(0xff5555), 0);
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
		snprintf(buf, sizeof(buf), "%d C", m->npu_temp_c);
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

	snprintf(buf, sizeof(buf), "OVERTEMP %d C", temp_c);
	set_label(lbl_overtemp, buf);
	lv_obj_clear_flag(lbl_overtemp, LV_OBJ_FLAG_HIDDEN);
}

void ui_clear_overtemp(void)
{
	lv_obj_add_flag(lbl_overtemp, LV_OBJ_FLAG_HIDDEN);
}
