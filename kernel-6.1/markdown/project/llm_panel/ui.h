/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_UI_H
#define LLM_PANEL_UI_H

#include "metrics.h"

void ui_create(void);
void ui_update_metrics(const struct llm_metrics *m);
void ui_show_overtemp(int temp_c);
void ui_clear_overtemp(void);

void ui_set_dialog(const char *text); /* 清空对话区；text 非空则作系统提示 */
void ui_append_dialog(const char *text); /* 系统提示气泡（居中） */
void ui_chat_user(const char *text); /* 右侧用户气泡 */
void ui_chat_assistant_begin(void); /* 左侧助手气泡（流式开始） */
void ui_chat_append_token(const char *text); /* 追加到当前助手气泡 */
void ui_chat_assistant_end(void);
void ui_set_ctx_tps(int ctx, int max_ctx, float tps);
void ui_set_model_status(const char *text);
void ui_set_ws_status(const char *text);
void ui_set_fps(float fps);

/* 目标刷新：30 或 60（设置页） */
int ui_fps_target_hz(void);

/* 0 = local, 1 = cloud. */
int ui_mode_is_cloud(void);

typedef void (*ui_action_fn)(void);
typedef void (*ui_mode_fn)(int cloud);
void ui_set_actions(ui_action_fn on_compress, ui_action_fn on_ask);
void ui_set_stop_action(ui_action_fn on_stop);
void ui_set_mode_hook(ui_mode_fn on_mode);

#endif
