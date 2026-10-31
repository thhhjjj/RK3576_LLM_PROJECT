/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 三页 UI：仪表盘 / 本地推理（默认） / 设置
 * 720×1280；底栏导航。
 */

#include "ui.h"

#include "backlight.h"
#include "cloud_sse.h"
#include "font.h"
#include "gpu_disp.h"
#include "ime/pinyin_api.h"

#include "lvgl/lvgl.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#define APP_VER_STR "0.9.23"

#define COL_BG 0x0c1218
#define COL_PANEL 0x161e28
#define COL_TEXT 0xe6edf3
#define COL_MUTED 0x8b9aab
#define COL_ACCENT 0x3d9a8b
#define COL_WARN 0xe8a04d
#define COL_DANGER 0xe85d5d
#define COL_BAR_BG 0x243040
#define COL_BUBBLE_USER 0x2a5f56
#define COL_BUBBLE_ASST 0x1a2430
#define CHAT_BUBBLE_MAX_W 520
#define CHAT_KEEP_MAX 48

#define NAV_H 88
#define CONTENT_H (1280 - NAV_H)
#define PAGE_W 720
#define KB_H 360
#define CAND_H 52
#define ASK_BAR_Y 1116
/* 键盘打开时藏底栏，键盘贴屏底；输入栏在候选条上方 */
#define ASK_BAR_Y_KB (1280 - KB_H - CAND_H - 64)
#define CHAT_LIST_Y 130
#define KB_ANIM_MS 260
#define SLIDE_MS 280
#define DRAG_LOCK_PX 12
#define SNAP_PX (PAGE_W / 5)
#define KB_Y_SHOWN (1280 - KB_H)
#define KB_Y_HIDDEN 1280

enum { PAGE_DASH = 0, PAGE_CHAT = 1, PAGE_SET = 2 };

static lv_obj_t *page_host; /* 裁剪容器，防邻页露边 */
static lv_obj_t *page_dash;
static lv_obj_t *page_chat;
static lv_obj_t *page_set;
static lv_obj_t *nav_bar;
static lv_obj_t *nav_btns[3];
static int cur_page = PAGE_CHAT;
static int slide_busy;
static lv_obj_t *slide_from;
static lv_obj_t *slide_to_obj;

/* 跟手：1=已锁定横向翻页；-1=交给纵向滚动 */
static int drag_down;
static int drag_lock;
static lv_coord_t drag_x0;
static lv_coord_t drag_y0;
static lv_coord_t drag_dx;
static int drag_nbr = -1;
static int ta_scroll_saved;

/* —— 仪表盘 —— */
static lv_obj_t *bar_mem, *bar_membus, *bar_cpu, *bar_gpu, *bar_npu;
static lv_obj_t *lbl_mem_v, *lbl_membus_v, *lbl_cpu_v, *lbl_gpu_v, *lbl_npu_v,
	*lbl_temp_v;
static lv_obj_t *lbl_dash_model;
static lv_obj_t *lbl_overtemp;

/* —— 聊天 —— */
static lv_obj_t *chat_list;
static lv_obj_t *stream_lab; /* 当前流式助手气泡内的 label */
static int stream_lead_ws; /* 丢掉助手气泡开头的空白/换行 */
static lv_obj_t *lbl_ctx, *lbl_tps, *lbl_fps, *lbl_ws, *lbl_chat_model;
static lv_obj_t *btn_compress, *btn_send, *btn_stop;
static lv_obj_t *ta_ask;
static lv_obj_t *kb;
static lv_obj_t *ime_py;
static lv_obj_t *cand_panel;
static int kb_open;
static int ask_bar_y_cur = ASK_BAR_Y;

/* —— 设置 —— */
static lv_obj_t *sw_mode;
static lv_obj_t *lbl_mode;
static lv_obj_t *ta_key;
static lv_obj_t *btn_key_save;
static lv_obj_t *sw_fps;
static lv_obj_t *lbl_fps_set;
static lv_obj_t *slider_bl;
static lv_obj_t *lbl_bl;
static int mode_cloud;
static int fps_target = 60;

static ui_action_fn act_compress;
static ui_ask_fn act_ask;
static ui_action_fn act_stop;
static ui_mode_fn act_mode;

static void kb_hide(void);
static void ask_bar_set_y(lv_coord_t y);

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

static lv_obj_t *page_at(int page)
{
	if (page == PAGE_DASH)
		return page_dash;
	if (page == PAGE_CHAT)
		return page_chat;
	return page_set;
}

static void set_nav_hl(int page)
{
	int i;

	for (i = 0; i < 3; i++) {
		if (!nav_btns[i])
			continue;
		lv_obj_set_style_bg_color(
			nav_btns[i],
			lv_color_hex(i == page ? COL_ACCENT : COL_PANEL), 0);
	}
}

/* 只用 translate_x 跟手/动画，布局 x 永远为 0，避免残留偏移 */
static int slide_anim_token;
static lv_coord_t pair_x0_a, pair_x1_a, pair_x0_b, pair_x1_b;
static lv_obj_t *pair_obj_a, *pair_obj_b;

static void dbg_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;
	FILE *k;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	/* 进内核环缓：adb shell dmesg | grep llm_debug */
	k = fopen("/dev/kmsg", "w");
	if (k) {
		fprintf(k, "<6>llm_debug: %s\n", buf);
		fclose(k);
	}
	fprintf(stderr, "llm_debug: %s\n", buf);
}

static void page_set_off(lv_obj_t *p, lv_coord_t off)
{
	if (!p)
		return;
	lv_obj_set_x(p, 0);
	lv_obj_set_style_translate_x(p, off, 0);
}

static lv_coord_t page_get_off(lv_obj_t *p)
{
	return p ? lv_obj_get_style_translate_x(p, 0) : 0;
}

static void pages_sync_idle(void)
{
	int i;

	lv_anim_del(&slide_anim_token, NULL);
	pair_obj_a = NULL;
	pair_obj_b = NULL;

	for (i = 0; i < 3; i++) {
		lv_obj_t *p = page_at(i);

		page_set_off(p, 0);
		lv_obj_set_pos(p, 0, 0);
		if (i == cur_page)
			lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
	}
	if (page_host)
		lv_obj_update_layout(page_host);

	slide_from = NULL;
	slide_to_obj = NULL;
	drag_nbr = -1;
	slide_busy = 0;
}

static void show_page(int page)
{
	cur_page = page;
	pages_sync_idle();
	set_nav_hl(page);
	kb_hide();
}

static void anim_pair_exec(void *var, int32_t v)
{
	(void)var;
	/* v: 0 → 256 */
	if (pair_obj_a)
		page_set_off(pair_obj_a,
			     pair_x0_a +
				     (lv_coord_t)(((int32_t)(pair_x1_a -
							     pair_x0_a) *
						   v) /
						  256));
	if (pair_obj_b)
		page_set_off(pair_obj_b,
			     pair_x0_b +
				     (lv_coord_t)(((int32_t)(pair_x1_b -
							     pair_x0_b) *
						   v) /
						  256));
}

static void sync_async_cb(void *user)
{
	(void)user;
	pages_sync_idle();
	dbg_log("synced async off d=%d c=%d s=%d", (int)page_get_off(page_dash),
		(int)page_get_off(page_chat), (int)page_get_off(page_set));
}

static void slide_ready(lv_anim_t *a)
{
	(void)a;
	dbg_log("ready cur=%d a_off=%d b_off=%d", cur_page,
		pair_obj_a ? (int)page_get_off(pair_obj_a) : -9999,
		pair_obj_b ? (int)page_get_off(pair_obj_b) : -9999);
	/* 先把当前页钉在 0，再异步清场 */
	if (pair_obj_a)
		page_set_off(pair_obj_a, pair_x1_a);
	if (pair_obj_b)
		page_set_off(pair_obj_b, pair_x1_b);
	page_set_off(page_at(cur_page), 0);
	pair_obj_a = NULL;
	pair_obj_b = NULL;
	lv_async_call(sync_async_cb, NULL);
}

static void anim_pair_to(lv_obj_t *a, lv_coord_t a_to, lv_obj_t *b,
			 lv_coord_t b_to)
{
	lv_anim_t anim;

	lv_anim_del(&slide_anim_token, NULL);
	pair_obj_a = a;
	pair_obj_b = b;
	pair_x0_a = page_get_off(a);
	pair_x1_a = a_to;
	pair_x0_b = page_get_off(b);
	pair_x1_b = b_to;

	lv_anim_init(&anim);
	lv_anim_set_var(&anim, &slide_anim_token);
	lv_anim_set_values(&anim, 0, 256);
	lv_anim_set_time(&anim, SLIDE_MS);
	lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&anim, anim_pair_exec);
	lv_anim_set_ready_cb(&anim, slide_ready);
	lv_anim_start(&anim);
}

static void ta_scroll_pause(int pause)
{
	if (!chat_list)
		return;
	if (pause) {
		ta_scroll_saved =
			lv_obj_has_flag(chat_list, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_clear_flag(chat_list, LV_OBJ_FLAG_SCROLLABLE);
	} else if (ta_scroll_saved) {
		lv_obj_add_flag(chat_list, LV_OBJ_FLAG_SCROLLABLE);
		ta_scroll_saved = 0;
	}
}

static int chat_row_holds_stream(lv_obj_t *row)
{
	lv_obj_t *p = stream_lab;

	while (p) {
		if (p == row)
			return 1;
		p = lv_obj_get_parent(p);
	}
	return 0;
}

static void chat_trim_old(void)
{
	while (chat_list && lv_obj_get_child_cnt(chat_list) > CHAT_KEEP_MAX) {
		lv_obj_t *old = lv_obj_get_child(chat_list, 0);

		if (!old)
			break;
		if (stream_lab && chat_row_holds_stream(old))
			break;
		lv_obj_del(old);
	}
}

static void chat_scroll_bottom(void)
{
	if (chat_list)
		lv_obj_scroll_to_y(chat_list, LV_COORD_MAX, LV_ANIM_OFF);
}

/* side: 0 助手左，1 用户右，2 系统居中 */
static lv_obj_t *chat_add_bubble(int side, const char *text, uint32_t bg)
{
	lv_obj_t *row;
	lv_obj_t *bubble;
	lv_obj_t *lab;

	if (!chat_list || !text)
		return NULL;

	row = lv_obj_create(chat_list);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	if (side == 1)
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START,
				      LV_FLEX_ALIGN_START);
	else if (side == 2)
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	else
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
				      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	bubble = lv_obj_create(row);
	lv_obj_set_width(bubble, LV_SIZE_CONTENT);
	lv_obj_set_height(bubble, LV_SIZE_CONTENT);
	lv_obj_set_style_max_width(bubble, CHAT_BUBBLE_MAX_W, 0);
	lv_obj_set_style_radius(bubble, side == 2 ? 8 : 14, 0);
	lv_obj_set_style_pad_ver(bubble, side == 2 ? 6 : 12, 0);
	lv_obj_set_style_pad_hor(bubble, side == 2 ? 10 : 14, 0);
	lv_obj_set_style_border_width(bubble, 0, 0);
	lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
	if (side == 2) {
		lv_obj_set_style_bg_opa(bubble, LV_OPA_20, 0);
		lv_obj_set_style_bg_color(bubble, lv_color_hex(COL_PANEL), 0);
	} else {
		lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(bubble, lv_color_hex(bg), 0);
	}

	lab = lv_label_create(bubble);
	lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(lab, LV_SIZE_CONTENT);
	lv_obj_set_style_max_width(lab, CHAT_BUBBLE_MAX_W - 28, 0);
	lv_label_set_text(lab, text[0] ? text : " ");
	style_text(lab, side == 2 ? font_small() : font_body(),
		   side == 2 ? COL_MUTED : COL_TEXT);

	chat_trim_old();
	chat_scroll_bottom();
	return lab;
}

static const char *trim_sys(const char *text, char *buf, size_t n)
{
	size_t i, j, len;

	if (!text || !buf || n < 2)
		return "";
	while (*text == '\n' || *text == ' ' || *text == '\t')
		text++;
	len = strlen(text);
	while (len > 0 &&
	       (text[len - 1] == '\n' || text[len - 1] == ' ' ||
		text[len - 1] == '\t'))
		len--;
	if (len >= 2 && text[0] == '[' && text[len - 1] == ']') {
		text++;
		len -= 2;
	}
	if (len >= n)
		len = n - 1;
	for (i = 0, j = 0; i < len && j + 1 < n; i++) {
		if (text[i] == '\n' && j > 0 && buf[j - 1] == '\n')
			continue;
		buf[j++] = text[i];
	}
	buf[j] = '\0';
	return buf;
}

static void drag_apply(lv_coord_t dx)
{
	lv_obj_t *cur = page_at(cur_page);
	int nbr;
	lv_coord_t min_dx = cur_page < PAGE_SET ? -PAGE_W : 0;
	lv_coord_t max_dx = cur_page > PAGE_DASH ? PAGE_W : 0;

	if (dx < min_dx)
		dx = min_dx;
	if (dx > max_dx)
		dx = max_dx;
	drag_dx = dx;
	page_set_off(cur, dx);

	if (dx < 0)
		nbr = cur_page + 1;
	else if (dx > 0)
		nbr = cur_page - 1;
	else
		nbr = -1;

	if (drag_nbr >= 0 && drag_nbr != nbr && drag_nbr != cur_page) {
		page_set_off(page_at(drag_nbr), 0);
		lv_obj_add_flag(page_at(drag_nbr), LV_OBJ_FLAG_HIDDEN);
	}
	drag_nbr = nbr;
	if (nbr < 0)
		return;
	page_set_off(page_at(nbr),
		     dx < 0 ? (lv_coord_t)(PAGE_W + dx) :
			      (lv_coord_t)(-PAGE_W + dx));
	lv_obj_clear_flag(page_at(nbr), LV_OBJ_FLAG_HIDDEN);
}

/* 松手吸附：切页或弹回 */
static void settle_to(int target)
{
	int from = cur_page;
	lv_obj_t *cur = page_at(from);
	lv_obj_t *nbr = drag_nbr >= 0 ? page_at(drag_nbr) : NULL;
	int dir;
	lv_coord_t coff;

	dbg_log("settle from=%d target=%d dx=%d nbr=%d off=%d", from, target,
		(int)drag_dx, drag_nbr, (int)page_get_off(cur));

	slide_busy = 1;
	if (target == from) {
		if (!nbr || drag_dx == 0) {
			pages_sync_idle();
			return;
		}
		slide_from = nbr;
		slide_to_obj = cur;
		anim_pair_to(cur, 0, nbr,
			     drag_dx < 0 ? (lv_coord_t)PAGE_W :
					   (lv_coord_t)(-PAGE_W));
		return;
	}

	/* dir>0 下一页从右侧进；dir<0 上一页从左侧进 */
	dir = target > from ? 1 : -1;
	if (!nbr) {
		nbr = page_at(target);
		drag_nbr = target;
	}
	coff = page_get_off(cur);
	page_set_off(nbr, (lv_coord_t)(coff + dir * PAGE_W));
	lv_obj_clear_flag(nbr, LV_OBJ_FLAG_HIDDEN);
	dbg_log("settle dir=%d cur_off=%d nbr_off=%d", dir, (int)coff,
		(int)page_get_off(nbr));
	slide_from = cur;
	slide_to_obj = nbr;
	cur_page = target;
	set_nav_hl(target);
	anim_pair_to(cur, (lv_coord_t)(-dir * PAGE_W), nbr, 0);
}

/* 底栏：非跟手，整页滑入 */
static void slide_to(int page)
{
	int from = cur_page;
	int dir;

	if (page == from || slide_busy || drag_down)
		return;
	if (page < PAGE_DASH || page > PAGE_SET)
		return;

	dir = page > from ? 1 : -1;
	drag_nbr = page;
	drag_dx = 0;
	page_set_off(page_at(from), 0);
	page_set_off(page_at(page), (lv_coord_t)(dir * PAGE_W));
	lv_obj_clear_flag(page_at(page), LV_OBJ_FLAG_HIDDEN);
	slide_busy = 1;
	slide_from = page_at(from);
	slide_to_obj = page_at(page);
	cur_page = page;
	set_nav_hl(page);
	anim_pair_to(slide_from, (lv_coord_t)(-dir * PAGE_W), slide_to_obj, 0);
}

static void on_nav(lv_event_t *e)
{
	int id = (int)(intptr_t)lv_event_get_user_data(e);

	slide_to(id);
}

static void on_drag(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_get_act();
	lv_point_t pt;
	lv_coord_t dx, dy;
	int target;

	if (!indev)
		return;

	if (code == LV_EVENT_PRESSED) {
		if (slide_busy)
			return;
		lv_indev_get_point(indev, &pt);
		drag_down = 1;
		drag_lock = 0;
		drag_x0 = pt.x;
		drag_y0 = pt.y;
		drag_dx = 0;
		drag_nbr = -1;
		return;
	}

	if (!drag_down)
		return;

	if (code == LV_EVENT_PRESSING) {
		if (slide_busy || drag_lock < 0)
			return;
		lv_indev_get_point(indev, &pt);
		dx = pt.x - drag_x0;
		dy = pt.y - drag_y0;
		if (!drag_lock) {
			if (LV_ABS(dx) < DRAG_LOCK_PX &&
			    LV_ABS(dy) < DRAG_LOCK_PX)
				return;
			if (LV_ABS(dy) > LV_ABS(dx)) {
				drag_lock = -1;
				return;
			}
			drag_lock = 1;
			ta_scroll_pause(1);
			kb_hide();
			dbg_log("lock horiz page=%d x0=%d", cur_page,
				(int)drag_x0);
		}
		drag_apply(dx);
		return;
	}

	/*
	 * 拖页时父对象在动，LVGL 常误发 PRESS_LOST → 提前 settle 导致归位错乱。
	 * 只认真正松手；PRESS_LOST 忽略（PRESS_LOCK 保住按下对象）。
	 */
	if (code == LV_EVENT_PRESS_LOST) {
		dbg_log("ignore PRESS_LOST lock=%d dx=%d", drag_lock,
			(int)drag_dx);
		return;
	}
	if (code != LV_EVENT_RELEASED)
		return;

	if (drag_lock == 1) {
		ta_scroll_pause(0);
		if (drag_dx <= -SNAP_PX && cur_page < PAGE_SET)
			target = cur_page + 1;
		else if (drag_dx >= SNAP_PX && cur_page > PAGE_DASH)
			target = cur_page - 1;
		else
			target = cur_page;
		dbg_log("release dx=%d -> target=%d", (int)drag_dx, target);
		settle_to(target);
	}
	drag_down = 0;
	drag_lock = 0;
}

static void attach_swipe(lv_obj_t *obj)
{
	lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);
	lv_obj_add_event_cb(obj, on_drag, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(obj, on_drag, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(obj, on_drag, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(obj, on_drag, LV_EVENT_PRESS_LOST, NULL);
}

static void ask_bar_set_y(lv_coord_t y)
{
	lv_coord_t chat_h;

	ask_bar_y_cur = y;
	if (btn_compress)
		lv_obj_set_y(btn_compress, y);
	if (ta_ask)
		lv_obj_set_y(ta_ask, y);
	if (btn_send)
		lv_obj_set_y(btn_send, y);
	if (btn_stop)
		lv_obj_set_y(btn_stop, y);
	if (chat_list) {
		chat_h = y - CHAT_LIST_Y - 12;
		if (chat_h < 200)
			chat_h = 200;
		lv_obj_set_height(chat_list, chat_h);
	}
}

static void ask_bar_anim_exec(void *var, int32_t v)
{
	(void)var;
	ask_bar_set_y((lv_coord_t)v);
}

static void cand_sync_to_kb(void)
{
	if (cand_panel && kb && !lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN))
		lv_obj_align_to(cand_panel, kb, LV_ALIGN_OUT_TOP_MID, 0, 0);
}

static void kb_place_abs(lv_coord_t y)
{
	if (!kb)
		return;
	/*
	 * keyboard 默认 BOTTOM 对齐；改用 TOP_LEFT + 绝对 y，
	 * 避免 set_y 被当成底边偏移、键落在导航底下。
	 */
	lv_obj_set_align(kb, LV_ALIGN_TOP_LEFT);
	lv_obj_set_pos(kb, 0, y);
	cand_sync_to_kb();
}

static void kb_y_anim_exec(void *var, int32_t v)
{
	(void)var;
	kb_place_abs((lv_coord_t)v);
}

static void nav_set_visible(int show)
{
	if (!nav_bar)
		return;
	if (show)
		lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(nav_bar, LV_OBJ_FLAG_HIDDEN);
}

static void kb_raise(void)
{
	if (ime_py) {
		lv_obj_clear_flag(ime_py, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(ime_py, 0, 0);
		/* 勿 HIDDEN：部分事件链依赖对象在树上可见态 */
		lv_obj_clear_flag(ime_py, LV_OBJ_FLAG_HIDDEN);
		lv_obj_move_background(ime_py);
	}
	if (cand_panel)
		lv_obj_move_foreground(cand_panel);
	if (kb)
		lv_obj_move_foreground(kb);
}

static void kb_hide_anim_ready(lv_anim_t *a)
{
	(void)a;
	if (kb)
		lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
	if (cand_panel)
		lv_obj_add_flag(cand_panel, LV_OBJ_FLAG_HIDDEN);
	nav_set_visible(1);
}

static void anim_y(void *var, int32_t from, int32_t to,
		   lv_anim_exec_xcb_t exec, uint32_t ms,
		   lv_anim_ready_cb_t ready)
{
	lv_anim_t a;

	lv_anim_init(&a);
	lv_anim_set_var(&a, var);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_time(&a, ms);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&a, exec);
	if (ready)
		lv_anim_set_ready_cb(&a, ready);
	lv_anim_start(&a);
}

static void kb_hide(void)
{
	lv_coord_t y0;

	if (!kb)
		return;
	lv_anim_del(kb, kb_y_anim_exec);
	if (ta_ask)
		lv_anim_del(ta_ask, ask_bar_anim_exec);

	if (!kb_open) {
		ask_bar_set_y(ASK_BAR_Y);
		nav_set_visible(1);
		return;
	}
	kb_open = 0;
	y0 = lv_obj_get_y(kb);
	/* 先落到屏外再藏，避免半截停在中间 */
	anim_y(kb, y0, KB_Y_HIDDEN, kb_y_anim_exec, KB_ANIM_MS,
	       kb_hide_anim_ready);
	anim_y(ta_ask ? ta_ask : kb, ask_bar_y_cur, ASK_BAR_Y,
	       ask_bar_anim_exec, KB_ANIM_MS, NULL);
}

static void kb_show_for(lv_obj_t *ta)
{
	lv_coord_t ask_to;

	if (!kb || !ta)
		return;

	lv_keyboard_set_textarea(kb, ta);
	nav_set_visible(0);

	ask_to = (ta == ta_ask) ? ASK_BAR_Y_KB : ASK_BAR_Y;
	if (ask_to > CONTENT_H - 64)
		ask_to = CONTENT_H - 64;

	/* 立刻可见：不依赖动画从屏外滑入（动画丢帧会「弹不出来」） */
	lv_anim_del(kb, kb_y_anim_exec);
	lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
	if (cand_panel)
		lv_obj_clear_flag(cand_panel, LV_OBJ_FLAG_HIDDEN);
	kb_place_abs(KB_Y_SHOWN);
	kb_raise();

	if (!kb_open) {
		if (ta_ask)
			lv_anim_del(ta_ask, ask_bar_anim_exec);
		anim_y(ta_ask ? ta_ask : kb, ask_bar_y_cur, ask_to,
		       ask_bar_anim_exec, KB_ANIM_MS, NULL);
		kb_open = 1;
	} else {
		ask_bar_set_y(ask_to);
	}
	/* set_textarea / IME 可能改对齐，再钉一次 */
	kb_place_abs(KB_Y_SHOWN);
	kb_raise();
	dbg_log("kb show y=%d ask=%d ta=%s", KB_Y_SHOWN, (int)ask_to,
		ta == ta_ask ? "ask" : "key");
}

static void on_ta_focused(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_target(e);

	kb_show_for(ta);
}

static void on_ta_pressed(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_target(e);

	/* PRESSED 最早；不依赖 FOCUSED/CLICKED */
	kb_show_for(ta);
}

static void on_ta_clicked(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_target(e);

	kb_show_for(ta);
}

static void on_kb_ready(lv_event_t *e)
{
	lv_obj_t *ta = kb ? lv_keyboard_get_textarea(kb) : NULL;

	(void)e;
	kb_hide();
	if (ta == ta_ask && act_ask) {
		const char *q = lv_textarea_get_text(ta_ask);

		if (q && q[0]) {
			act_ask(q);
			lv_textarea_set_text(ta_ask, "");
		}
	}
}

static void on_kb_cancel(lv_event_t *e)
{
	(void)e;
	kb_hide();
}

static void on_compress(lv_event_t *e)
{
	(void)e;
	if (act_compress)
		act_compress();
}

static void on_send(lv_event_t *e)
{
	const char *q;

	(void)e;
	if (!ta_ask || !act_ask)
		return;
	q = lv_textarea_get_text(ta_ask);
	if (!q || !q[0]) {
		ui_append_dialog("请输入问题");
		return;
	}
	act_ask(q);
	lv_textarea_set_text(ta_ask, "");
	kb_hide();
}

static void on_stop(lv_event_t *e)
{
	(void)e;
	if (act_stop)
		act_stop();
}

static void on_mode(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target(e);

	mode_cloud = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
	set_label(lbl_mode, mode_cloud ? "云端 DeepSeek" : "本地 RKLLM");
	if (act_mode)
		act_mode(mode_cloud);
}

static void on_key_save(lv_event_t *e)
{
	const char *k;

	(void)e;
	if (!ta_key)
		return;
	k = lv_textarea_get_text(ta_key);
	cloud_sse_set_key(k);
	/* 不清空输入框，避免误触；只提示是否已有 key */
	ui_append_dialog(cloud_sse_has_key() ? "API Key 已写入内存（不落盘）" :
					       "API Key 已清空");
}

static void on_fps_sw(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target(e);

	fps_target = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 60 : 30;
	set_label(lbl_fps_set, fps_target >= 60 ? "目标 60 fps" : "目标 30 fps");
	gpu_disp_set_refr_60hz();
	if (fps_target <= 30) {
		lv_disp_t *d = lv_disp_get_default();

		if (d && d->refr_timer)
			lv_timer_set_period(d->refr_timer, 33);
	}
}

static void on_bl_slider(lv_event_t *e)
{
	lv_obj_t *s = lv_event_get_target(e);
	int pct = (int)lv_slider_get_value(s);
	char buf[32];

	backlight_set_pct(pct);
	snprintf(buf, sizeof(buf), "亮度 %d%%", pct);
	set_label(lbl_bl, buf);
}

static lv_obj_t *make_page(lv_obj_t *parent)
{
	lv_obj_t *p = lv_obj_create(parent);

	lv_obj_set_size(p, 720, CONTENT_H);
	lv_obj_set_pos(p, 0, 0);
	lv_obj_set_style_bg_color(p, lv_color_hex(COL_BG), 0);
	lv_obj_set_style_border_width(p, 0, 0);
	lv_obj_set_style_pad_all(p, 0, 0);
	lv_obj_set_style_radius(p, 0, 0);
	lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
	return p;
}

static lv_obj_t *add_dash_row(lv_obj_t *parent, const char *title, int y,
			      lv_obj_t **bar_out, lv_obj_t **val_out)
{
	lv_obj_t *t = lv_label_create(parent);
	lv_obj_t *bar;
	lv_obj_t *v;

	lv_label_set_text(t, title);
	style_text(t, font_body(), COL_MUTED);
	lv_obj_set_pos(t, 28, y);

	v = lv_label_create(parent);
	lv_label_set_text(v, "—");
	style_text(v, font_title(), COL_TEXT);
	lv_obj_set_pos(v, 28, y + 36);
	*val_out = v;

	bar = lv_bar_create(parent);
	lv_obj_set_size(bar, 664, 18);
	lv_obj_set_pos(bar, 28, y + 88);
	lv_bar_set_range(bar, 0, 100);
	lv_bar_set_value(bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BAR_BG), LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ACCENT),
				  LV_PART_INDICATOR);
	lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
	lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
	*bar_out = bar;
	return bar;
}

static void build_dash(lv_obj_t *parent)
{
	lv_obj_t *title = lv_label_create(parent);

	lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(parent, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);

	lv_label_set_text(title, "仪表盘");
	style_text(title, font_title(), COL_TEXT);
	lv_obj_set_pos(title, 28, 24);

	lbl_dash_model = lv_label_create(parent);
	lv_label_set_text(lbl_dash_model, "模型: —");
	style_text(lbl_dash_model, font_small(), COL_MUTED);
	lv_obj_set_pos(lbl_dash_model, 28, 72);

	lbl_ws = lv_label_create(parent);
	lv_label_set_text(lbl_ws, "WS —");
	style_text(lbl_ws, font_small(), COL_MUTED);
	lv_obj_align(lbl_ws, LV_ALIGN_TOP_RIGHT, -24, 56);

	add_dash_row(parent, "内存占用", 120, &bar_mem, &lbl_mem_v);
	add_dash_row(parent, "内存总线占用", 260, &bar_membus, &lbl_membus_v);
	add_dash_row(parent, "CPU 负载", 400, &bar_cpu, &lbl_cpu_v);
	add_dash_row(parent, "GPU 负载", 540, &bar_gpu, &lbl_gpu_v);
	add_dash_row(parent, "NPU 负载", 680, &bar_npu, &lbl_npu_v);

	{
		lv_obj_t *t = lv_label_create(parent);

		lv_label_set_text(t, "NPU 温度");
		style_text(t, font_body(), COL_MUTED);
		lv_obj_set_pos(t, 28, 820);
		lbl_temp_v = lv_label_create(parent);
		lv_label_set_text(lbl_temp_v, "— ℃");
		style_text(lbl_temp_v, font_title(), COL_TEXT);
		lv_obj_set_pos(lbl_temp_v, 28, 856);
	}

	lbl_overtemp = lv_label_create(parent);
	lv_label_set_text(lbl_overtemp, "");
	style_text(lbl_overtemp, font_body(), COL_DANGER);
	lv_obj_set_pos(lbl_overtemp, 28, 920);
	lv_obj_add_flag(lbl_overtemp, LV_OBJ_FLAG_HIDDEN);
}

static void build_chat(lv_obj_t *parent)
{
	lv_obj_t *title = lv_label_create(parent);

	lv_label_set_text(title, "本地推理");
	style_text(title, font_title(), COL_TEXT);
	lv_obj_set_pos(title, 28, 20);

	lbl_chat_model = lv_label_create(parent);
	lv_label_set_text(lbl_chat_model, "模型: —");
	style_text(lbl_chat_model, font_small(), COL_MUTED);
	lv_obj_set_pos(lbl_chat_model, 200, 28);

	{
		lv_obj_t *t = lv_label_create(parent);

		lv_label_set_text(t, "上下文");
		style_text(t, font_small(), COL_MUTED);
		lv_obj_set_pos(t, 28, 88);
		lbl_ctx = lv_label_create(parent);
		lv_label_set_text(lbl_ctx, "—");
		style_text(lbl_ctx, font_body(), COL_TEXT);
		lv_obj_set_pos(lbl_ctx, 120, 84);
	}
	{
		lv_obj_t *t = lv_label_create(parent);

		lv_label_set_text(t, "token/s");
		style_text(t, font_small(), COL_MUTED);
		lv_obj_set_pos(t, 320, 88);
		lbl_tps = lv_label_create(parent);
		lv_label_set_text(lbl_tps, "—");
		style_text(lbl_tps, font_body(), COL_ACCENT);
		lv_obj_set_pos(lbl_tps, 420, 84);
	}

	chat_list = lv_obj_create(parent);
	lv_obj_set_size(chat_list, 664, ASK_BAR_Y - CHAT_LIST_Y - 12);
	lv_obj_set_pos(chat_list, 28, CHAT_LIST_Y);
	lv_obj_set_style_bg_color(chat_list, lv_color_hex(COL_BG), 0);
	lv_obj_set_style_bg_opa(chat_list, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(chat_list, 0, 0);
	lv_obj_set_style_pad_all(chat_list, 8, 0);
	lv_obj_set_style_pad_row(chat_list, 12, 0);
	lv_obj_set_style_radius(chat_list, 0, 0);
	lv_obj_set_flex_flow(chat_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(chat_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(chat_list, LV_SCROLLBAR_MODE_AUTO);
	/* 对话区也跟手：横滑翻页，竖滑仍滚动 */
	attach_swipe(chat_list);

	btn_compress = lv_btn_create(parent);
	lv_obj_set_size(btn_compress, 90, 56);
	lv_obj_set_pos(btn_compress, 28, ASK_BAR_Y);
	lv_obj_set_style_bg_color(btn_compress, lv_color_hex(COL_PANEL), 0);
	lv_obj_add_event_cb(btn_compress, on_compress, LV_EVENT_CLICKED, NULL);
	{
		lv_obj_t *lab = lv_label_create(btn_compress);
		lv_label_set_text(lab, "压缩");
		style_text(lab, font_body(), COL_TEXT);
		lv_obj_center(lab);
	}

	ta_ask = lv_textarea_create(parent);
	lv_obj_set_size(ta_ask, 360, 56);
	lv_obj_set_pos(ta_ask, 130, ASK_BAR_Y);
	lv_textarea_set_one_line(ta_ask, true);
	lv_textarea_set_max_length(ta_ask, 512);
	lv_textarea_set_placeholder_text(ta_ask, "输入问题…");
	lv_obj_set_style_bg_color(ta_ask, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_style_border_width(ta_ask, 0, 0);
	lv_obj_set_style_text_font(ta_ask, font_body(), 0);
	lv_obj_set_style_text_color(ta_ask, lv_color_hex(COL_TEXT), 0);
	lv_obj_set_style_pad_ver(ta_ask, 12, 0);
	lv_obj_add_event_cb(ta_ask, on_ta_pressed, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(ta_ask, on_ta_focused, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(ta_ask, on_ta_clicked, LV_EVENT_CLICKED, NULL);
	lv_obj_add_flag(ta_ask, LV_OBJ_FLAG_CLICKABLE);

	btn_send = lv_btn_create(parent);
	lv_obj_set_size(btn_send, 90, 56);
	lv_obj_set_pos(btn_send, 502, ASK_BAR_Y);
	lv_obj_set_style_bg_color(btn_send, lv_color_hex(COL_ACCENT), 0);
	lv_obj_add_event_cb(btn_send, on_send, LV_EVENT_CLICKED, NULL);
	{
		lv_obj_t *lab = lv_label_create(btn_send);
		lv_label_set_text(lab, "发送");
		style_text(lab, font_body(), COL_TEXT);
		lv_obj_center(lab);
	}

	btn_stop = lv_btn_create(parent);
	lv_obj_set_size(btn_stop, 90, 56);
	lv_obj_set_pos(btn_stop, 602, ASK_BAR_Y);
	lv_obj_set_style_bg_color(btn_stop, lv_color_hex(0x4a3030), 0);
	lv_obj_add_event_cb(btn_stop, on_stop, LV_EVENT_CLICKED, NULL);
	{
		lv_obj_t *lab = lv_label_create(btn_stop);
		lv_label_set_text(lab, "停止");
		style_text(lab, font_body(), COL_TEXT);
		lv_obj_center(lab);
	}
}

static int read_os_release_field(const char *key, char *out, size_t out_sz)
{
	FILE *f = fopen("/etc/os-release", "r");
	char line[256];
	size_t klen;

	if (!f || !out || out_sz < 2)
		return -1;
	klen = strlen(key);
	while (fgets(line, sizeof(line), f)) {
		char *v, *e;

		if (strncmp(line, key, klen) != 0 || line[klen] != '=')
			continue;
		v = line + klen + 1;
		if (*v == '"') {
			v++;
			e = strchr(v, '"');
			if (e)
				*e = '\0';
		} else {
			e = strchr(v, '\n');
			if (e)
				*e = '\0';
		}
		strncpy(out, v, out_sz - 1);
		out[out_sz - 1] = '\0';
		fclose(f);
		return 0;
	}
	fclose(f);
	return -1;
}

static void add_info_line(lv_obj_t *parent, int y, const char *text)
{
	lv_obj_t *lab = lv_label_create(parent);

	lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(lab, 664);
	lv_label_set_text(lab, text);
	style_text(lab, font_body(), COL_TEXT);
	lv_obj_set_pos(lab, 28, y);
}

static void build_set(lv_obj_t *parent)
{
	lv_obj_t *title = lv_label_create(parent);
	int y = 24;

	lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(parent, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);

	lv_label_set_text(title, "设置");
	style_text(title, font_title(), COL_TEXT);
	lv_obj_set_pos(title, 28, y);
	y += 72;

	{
		lv_obj_t *t = lv_label_create(parent);

		lv_label_set_text(t, "推理通路");
		style_text(t, font_body(), COL_MUTED);
		lv_obj_set_pos(t, 28, y);
		y += 40;
		sw_mode = lv_switch_create(parent);
		lv_obj_set_pos(sw_mode, 28, y);
		lv_obj_add_event_cb(sw_mode, on_mode, LV_EVENT_VALUE_CHANGED,
				    NULL);
		lbl_mode = lv_label_create(parent);
		lv_label_set_text(lbl_mode, "本地 RKLLM");
		style_text(lbl_mode, font_body(), COL_TEXT);
		lv_obj_set_pos(lbl_mode, 120, y + 4);
		y += 72;
	}

	{
		lv_obj_t *t = lv_label_create(parent);

		lv_label_set_text(t, "云端 API Key（仅内存，不落盘）");
		style_text(t, font_body(), COL_MUTED);
		lv_obj_set_pos(t, 28, y);
		y += 40;
		ta_key = lv_textarea_create(parent);
		lv_obj_set_size(ta_key, 500, 56);
		lv_obj_set_pos(ta_key, 28, y);
		lv_textarea_set_one_line(ta_key, true);
		lv_textarea_set_password_mode(ta_key, true);
		lv_textarea_set_placeholder_text(ta_key, "sk-…");
		lv_obj_set_style_bg_color(ta_key, lv_color_hex(COL_PANEL), 0);
		lv_obj_set_style_text_font(ta_key, font_body(), 0);
		lv_obj_add_event_cb(ta_key, on_ta_pressed, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(ta_key, on_ta_focused, LV_EVENT_FOCUSED, NULL);
		lv_obj_add_event_cb(ta_key, on_ta_clicked, LV_EVENT_CLICKED, NULL);
		lv_obj_add_flag(ta_key, LV_OBJ_FLAG_CLICKABLE);
		btn_key_save = lv_btn_create(parent);
		lv_obj_set_size(btn_key_save, 120, 56);
		lv_obj_set_pos(btn_key_save, 548, y);
		lv_obj_set_style_bg_color(btn_key_save, lv_color_hex(COL_ACCENT),
					 0);
		lv_obj_add_event_cb(btn_key_save, on_key_save, LV_EVENT_CLICKED,
				    NULL);
		{
			lv_obj_t *lab = lv_label_create(btn_key_save);
			lv_label_set_text(lab, "保存");
			style_text(lab, font_body(), COL_TEXT);
			lv_obj_center(lab);
		}
		y += 88;
	}

	{
		lv_obj_t *t = lv_label_create(parent);

		lv_label_set_text(t, "界面帧率");
		style_text(t, font_body(), COL_MUTED);
		lv_obj_set_pos(t, 28, y);
		y += 40;
		sw_fps = lv_switch_create(parent);
		lv_obj_add_state(sw_fps, LV_STATE_CHECKED); /* 默认 60 */
		lv_obj_set_pos(sw_fps, 28, y);
		lv_obj_add_event_cb(sw_fps, on_fps_sw, LV_EVENT_VALUE_CHANGED,
				    NULL);
		lbl_fps_set = lv_label_create(parent);
		lv_label_set_text(lbl_fps_set, "目标 60 fps");
		style_text(lbl_fps_set, font_body(), COL_TEXT);
		lv_obj_set_pos(lbl_fps_set, 120, y + 4);
		y += 72;
	}

	{
		lv_obj_t *t = lv_label_create(parent);

		lv_label_set_text(t, "背光亮度");
		style_text(t, font_body(), COL_MUTED);
		lv_obj_set_pos(t, 28, y);
		y += 40;
		slider_bl = lv_slider_create(parent);
		lv_obj_set_size(slider_bl, 664, 24);
		lv_obj_set_pos(slider_bl, 28, y);
		lv_slider_set_range(slider_bl, 20, 100);
		lv_slider_set_value(slider_bl, backlight_get_pct(), LV_ANIM_OFF);
		lv_obj_set_style_bg_color(slider_bl, lv_color_hex(COL_BAR_BG),
					 LV_PART_MAIN);
		lv_obj_set_style_bg_color(slider_bl, lv_color_hex(COL_WARN),
					 LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(slider_bl, lv_color_hex(COL_TEXT),
					 LV_PART_KNOB);
		lv_obj_add_event_cb(slider_bl, on_bl_slider, LV_EVENT_VALUE_CHANGED,
				    NULL);
		y += 48;
		lbl_bl = lv_label_create(parent);
		{
			char buf[32];

			snprintf(buf, sizeof(buf), "亮度 %d%%",
				 backlight_get_pct());
			lv_label_set_text(lbl_bl, buf);
		}
		style_text(lbl_bl, font_body(), COL_TEXT);
		lv_obj_set_pos(lbl_bl, 28, y);
		y += 64;
	}

	{
		lv_obj_t *t = lv_label_create(parent);
		struct utsname uts;
		char line[192];
		char pretty[96];
		char rkinfo[128];

		lv_label_set_text(t, "设备信息");
		style_text(t, font_body(), COL_MUTED);
		lv_obj_set_pos(t, 28, y);
		y += 40;

		if (uname(&uts) == 0) {
			int maj = 0, min = 0;

			if (sscanf(uts.release, "%d.%d", &maj, &min) >= 2)
				snprintf(line, sizeof(line),
					 "内核：linux_kernel%d.%d", maj, min);
			else
				snprintf(line, sizeof(line),
					 "内核：linux_kernel%s", uts.release);
		} else {
			snprintf(line, sizeof(line), "内核：linux_kernel6.1");
		}
		add_info_line(parent, y, line);
		y += 40;

		pretty[0] = '\0';
		if (read_os_release_field("PRETTY_NAME", pretty,
					  sizeof(pretty)) != 0)
			snprintf(pretty, sizeof(pretty), "Buildroot");
		snprintf(line, sizeof(line), "rootfs：%s", pretty);
		add_info_line(parent, y, line);
		y += 40;

		rkinfo[0] = '\0';
		if (read_os_release_field("RK_BUILD_INFO", rkinfo,
					  sizeof(rkinfo)) == 0 &&
		    rkinfo[0]) {
			snprintf(line, sizeof(line), "构建：%s", rkinfo);
			add_info_line(parent, y, line);
			y += 48;
		}

		add_info_line(parent, y, "模型：Qwen3.5_2B");
		y += 40;

		snprintf(line, sizeof(line), "程序：vtest %s", APP_VER_STR);
		add_info_line(parent, y, line);
		y += 48;
	}
}

static void build_nav(lv_obj_t *scr)
{
	static const char *names[3] = { "仪表盘", "推理", "设置" };
	int i;

	nav_bar = lv_obj_create(scr);
	lv_obj_set_size(nav_bar, 720, NAV_H);
	lv_obj_set_pos(nav_bar, 0, CONTENT_H);
	lv_obj_set_style_bg_color(nav_bar, lv_color_hex(0x0a0e14), 0);
	lv_obj_set_style_border_width(nav_bar, 0, 0);
	lv_obj_set_style_pad_all(nav_bar, 0, 0);
	lv_obj_set_style_radius(nav_bar, 0, 0);
	lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);

	for (i = 0; i < 3; i++) {
		lv_obj_t *b = lv_btn_create(nav_bar);
		lv_obj_t *lab;

		lv_obj_set_size(b, 220, 64);
		lv_obj_set_pos(b, 20 + i * 233, 12);
		lv_obj_set_style_radius(b, 8, 0);
		lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL), 0);
		lv_obj_add_event_cb(b, on_nav, LV_EVENT_CLICKED,
				    (void *)(intptr_t)i);
		lab = lv_label_create(b);
		lv_label_set_text(lab, names[i]);
		style_text(lab, font_body(), COL_TEXT);
		lv_obj_center(lab);
		nav_btns[i] = b;
	}
}

void ui_set_actions(ui_action_fn on_compress_fn, ui_ask_fn on_ask_fn)
{
	act_compress = on_compress_fn;
	act_ask = on_ask_fn;
}

void ui_set_stop_action(ui_action_fn on_stop_fn)
{
	act_stop = on_stop_fn;
}

void ui_set_mode_hook(ui_mode_fn on_mode_fn)
{
	act_mode = on_mode_fn;
}

int ui_mode_is_cloud(void)
{
	return mode_cloud;
}

int ui_fps_target_hz(void)
{
	return fps_target;
}

void ui_create(void)
{
	lv_obj_t *scr = lv_scr_act();

	backlight_init();
	lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);

	page_host = lv_obj_create(scr);
	lv_obj_set_size(page_host, PAGE_W, CONTENT_H);
	lv_obj_set_pos(page_host, 0, 0);
	lv_obj_set_style_bg_opa(page_host, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(page_host, 0, 0);
	lv_obj_set_style_pad_all(page_host, 0, 0);
	lv_obj_set_style_radius(page_host, 0, 0);
	lv_obj_clear_flag(page_host, LV_OBJ_FLAG_SCROLLABLE);

	page_dash = make_page(page_host);
	page_chat = make_page(page_host);
	page_set = make_page(page_host);
	build_dash(page_dash);
	build_chat(page_chat);
	build_set(page_set);
	build_nav(scr);

	/* 帧率固定屏右上，不随翻页滑动 */
	lbl_fps = lv_label_create(scr);
	lv_label_set_text(lbl_fps, "— fps");
	style_text(lbl_fps, font_body(), COL_TEXT);
	lv_obj_align(lbl_fps, LV_ALIGN_TOP_RIGHT, -24, 22);
	lv_obj_add_flag(lbl_fps, LV_OBJ_FLAG_FLOATING);
	lv_obj_clear_flag(lbl_fps, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_move_foreground(lbl_fps);

	attach_swipe(page_dash);
	attach_swipe(page_chat);
	attach_swipe(page_set);
	/* 不挂 scr：避免与子页重复收手势 */

	/* 软键盘：绝对坐标 + 打开时藏底栏，避免下排键被导航/IME 层吃掉 */
	kb = lv_keyboard_create(scr);
	lv_obj_set_size(kb, PAGE_W, KB_H);
	kb_place_abs(KB_Y_HIDDEN);
	lv_obj_set_style_bg_color(kb, lv_color_hex(COL_BAR_BG), 0);
	lv_obj_set_style_text_font(kb, font_body(), 0);
	lv_obj_set_style_text_font(kb, font_body(), LV_PART_ITEMS);
	lv_obj_add_event_cb(kb, on_kb_ready, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(kb, on_kb_cancel, LV_EVENT_CANCEL, NULL);
	lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(kb, LV_OBJ_FLAG_SCROLLABLE);

	ime_py = lv_ime_pinyin_create(scr);
	if (ime_py) {
		lv_obj_set_size(ime_py, 0, 0);
		lv_obj_clear_flag(ime_py, LV_OBJ_FLAG_CLICKABLE);
		lv_ime_pinyin_set_keyboard(ime_py, kb);
		lv_obj_set_style_text_font(ime_py, font_body(), 0);
		lv_obj_set_size(ime_py, 0, 0);
		lv_obj_clear_flag(ime_py, LV_OBJ_FLAG_CLICKABLE);
		cand_panel = lv_ime_pinyin_get_cand_panel(ime_py);
		if (cand_panel) {
			lv_obj_set_size(cand_panel, PAGE_W, CAND_H);
			lv_obj_set_style_text_font(cand_panel, font_body(), 0);
			lv_obj_set_style_bg_color(cand_panel,
						  lv_color_hex(COL_PANEL), 0);
			lv_obj_set_style_bg_opa(cand_panel, LV_OPA_COVER, 0);
			lv_obj_add_flag(cand_panel, LV_OBJ_FLAG_HIDDEN);
		}
	}

	ask_bar_set_y(ASK_BAR_Y);
	dbg_log("ui up");

	/* 上电默认推理页；也可底栏点选或左右滑 */
	show_page(PAGE_CHAT);
}

void ui_update_metrics(const struct llm_metrics *m)
{
	char buf[128];
	unsigned pct;

	if (m->valid & METRICS_HAVE_MEM) {
		unsigned long long used =
			m->mem_total_kb > m->mem_available_kb ?
				m->mem_total_kb - m->mem_available_kb :
				0;
		unsigned long long tot = m->mem_total_kb ? m->mem_total_kb : 1;

		pct = (unsigned)((used * 100) / tot);
		if (pct > 100)
			pct = 100;
		snprintf(buf, sizeof(buf), "%llu / %llu MB  (%u%%)",
			 used / 1024, tot / 1024, pct);
		set_label(lbl_mem_v, buf);
		if (bar_mem)
			lv_bar_set_value(bar_mem, (int32_t)pct, LV_ANIM_OFF);
	}
	if (m->valid & METRICS_HAVE_MEM_BUS) {
		snprintf(buf, sizeof(buf), "%u%%", m->mem_bus_pct);
		set_label(lbl_membus_v, buf);
		if (bar_membus)
			lv_bar_set_value(bar_membus, (int32_t)m->mem_bus_pct,
					 LV_ANIM_OFF);
	}
	if (m->valid & METRICS_HAVE_CPU) {
		snprintf(buf, sizeof(buf), "%u%%", m->cpu_pct);
		set_label(lbl_cpu_v, buf);
		if (bar_cpu)
			lv_bar_set_value(bar_cpu, (int32_t)m->cpu_pct,
					 LV_ANIM_OFF);
	}
	if (m->valid & METRICS_HAVE_GPU) {
		snprintf(buf, sizeof(buf), "%u%%", m->gpu_load_pct);
		set_label(lbl_gpu_v, buf);
		if (bar_gpu)
			lv_bar_set_value(bar_gpu, (int32_t)m->gpu_load_pct,
					 LV_ANIM_OFF);
	}
	if (m->valid & METRICS_HAVE_NPU_LOAD) {
		snprintf(buf, sizeof(buf), "%u%%", m->npu_load_pct);
		set_label(lbl_npu_v, buf);
		if (bar_npu)
			lv_bar_set_value(bar_npu, (int32_t)m->npu_load_pct,
					 LV_ANIM_OFF);
	}
	if (m->valid & METRICS_HAVE_NPU_TEMP) {
		snprintf(buf, sizeof(buf), "%d ℃", m->npu_temp_c);
		set_label(lbl_temp_v, buf);
		if (m->npu_temp_c >= 85)
			style_text(lbl_temp_v, font_title(), COL_DANGER);
		else if (m->npu_temp_c >= 70)
			style_text(lbl_temp_v, font_title(), COL_WARN);
		else
			style_text(lbl_temp_v, font_title(), COL_TEXT);
	}
}

void ui_show_overtemp(int temp_c)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "超温告警 %d ℃", temp_c);
	set_label(lbl_overtemp, buf);
	lv_obj_clear_flag(lbl_overtemp, LV_OBJ_FLAG_HIDDEN);
}

void ui_clear_overtemp(void)
{
	lv_obj_add_flag(lbl_overtemp, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_dialog(const char *text)
{
	if (!chat_list)
		return;
	stream_lab = NULL;
	lv_obj_clean(chat_list);
	if (text && text[0])
		ui_append_dialog(text);
}

void ui_append_dialog(const char *text)
{
	char buf[512];
	const char *t;

	if (!text || !text[0])
		return;
	t = trim_sys(text, buf, sizeof(buf));
	if (!t[0])
		return;
	chat_add_bubble(2, t, COL_PANEL);
}

/* UTF-8 全角冒号 U+FF1A */
static int is_fw_colon(const char *p)
{
	const unsigned char *u = (const unsigned char *)p;

	return u[0] == 0xEF && u[1] == 0xBC && u[2] == 0x9A;
}

static int role_sep_at(const char *p)
{
	if (!p || !*p)
		return 0;
	if (*p == ':' || *p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
		return 1;
	return is_fw_colon(p) ? 3 : 0;
}

static int role_word_len(const char *s)
{
	static const char *roles[] = { "assistant", "Assistant", "ASSISTANT",
				       "user", "User", "system", "System",
				       NULL };
	int i;

	for (i = 0; roles[i]; i++) {
		size_t n = strlen(roles[i]);

		if (strncmp(s, roles[i], n) == 0) {
			char c = s[n];

			if (c == '\0' || role_sep_at(s + n))
				return (int)n;
		}
	}
	return 0;
}

/* 去掉尾部角色名（模型常泄漏下一轮 "assistant"） */
static void trim_role_tail_buf(char *s)
{
	size_t n;
	int again = 1;

	if (!s)
		return;
	while (again) {
		again = 0;
		n = strlen(s);
		while (n > 0 &&
		       (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
			s[n - 1] == '\t')) {
			s[--n] = '\0';
			again = 1;
		}
		{
			size_t i = n;

			while (i > 0 && s[i - 1] != '\n' && s[i - 1] != ' ' &&
			       s[i - 1] != '\t')
				i--;
			if (role_word_len(s + i) > 0 &&
			    (size_t)role_word_len(s + i) == n - i) {
				s[i] = '\0';
				again = 1;
			}
		}
	}
}

/* token 去掉空白后是否只剩角色名（整段丢掉，不上屏） */
static int token_is_role_only(const char *text)
{
	const char *p = text;
	int rw;

	if (!p)
		return 1;
	while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
		p++;
	rw = role_word_len(p);
	if (rw <= 0)
		return 0;
	p += rw;
	for (;;) {
		int skip = role_sep_at(p);

		if (!skip)
			break;
		p += skip;
	}
	return *p == '\0';
}

static void stream_apply_clean(void)
{
	const char *cur;
	char buf[4096];

	if (!stream_lab)
		return;
	cur = lv_label_get_text(stream_lab);
	if (!cur || strcmp(cur, "…") == 0)
		return;
	strncpy(buf, cur, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	trim_role_tail_buf(buf);
	if (strcmp(buf, cur) != 0) {
		dbg_log("asst trim '%s' -> '%s'", cur, buf[0] ? buf : "(empty)");
		lv_label_set_text(stream_lab, buf[0] ? buf : "…");
		if (!buf[0])
			stream_lead_ws = 1;
	}
}

void ui_chat_user(const char *text)
{
	if (!text || !text[0])
		return;
	stream_lab = NULL;
	stream_lead_ws = 0;
	dbg_log("user bubble len=%zu", strlen(text));
	chat_add_bubble(1, text, COL_BUBBLE_USER);
}

void ui_chat_assistant_begin(void)
{
	stream_lab = chat_add_bubble(0, "…", COL_BUBBLE_ASST);
	stream_lead_ws = 1;
	dbg_log("asst begin");
}

void ui_chat_append_token(const char *text)
{
	const char *cur;
	const char *p = text;
	int rw;
	char preview[48];
	size_t i;

	if (!text || !text[0])
		return;
	if (!stream_lab)
		ui_chat_assistant_begin();
	if (!stream_lab)
		return;

	/* 纯 assistant/user 整段丢弃，绝不先画再擦 */
	if (token_is_role_only(text)) {
		for (i = 0; i < sizeof(preview) - 1 && text[i]; i++)
			preview[i] = (text[i] >= 32 && text[i] < 127) ?
					     text[i] :
					     '.';
		preview[i] = '\0';
		dbg_log("asst drop role-only '%s'", preview);
		return;
	}

	if (stream_lead_ws) {
		while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
			p++;
		rw = role_word_len(p);
		if (rw > 0) {
			dbg_log("asst drop lead role %d bytes", rw);
			p += rw;
			for (;;) {
				int skip = role_sep_at(p);

				if (!skip)
					break;
				p += skip;
			}
		}
		if (!*p)
			return;
		if (token_is_role_only(p)) {
			dbg_log("asst drop lead role-only");
			return;
		}
		stream_lead_ws = 0;
	}

	cur = lv_label_get_text(stream_lab);
	if (cur && strcmp(cur, "…") == 0)
		lv_label_set_text(stream_lab, p);
	else
		lv_label_ins_text(stream_lab, LV_LABEL_POS_LAST, p);
	/* 若本段尾巴拼出了 assistant，立刻裁掉，同一帧内不上屏 */
	stream_apply_clean();
	chat_scroll_bottom();
}

void ui_chat_assistant_end(void)
{
	const char *cur;
	char buf[4096];

	if (stream_lab) {
		stream_apply_clean();
		cur = lv_label_get_text(stream_lab);
		if (!cur || strcmp(cur, "…") == 0 || !cur[0]) {
			dbg_log("asst end empty");
			lv_label_set_text(stream_lab, "（空）");
		} else {
			strncpy(buf, cur, sizeof(buf) - 1);
			buf[sizeof(buf) - 1] = '\0';
			dbg_log("asst end len=%zu", strlen(buf));
			lv_label_set_text(stream_lab, buf);
		}
	}
	stream_lab = NULL;
	stream_lead_ws = 0;
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
	set_label(lbl_dash_model, buf);
	set_label(lbl_chat_model, buf);
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
		lv_obj_align(lbl_fps, LV_ALIGN_TOP_RIGHT, -24, 22);
}
