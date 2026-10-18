/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_UI_H
#define LLM_PANEL_UI_H

#include "metrics.h"

void ui_create(void);
void ui_update_metrics(const struct llm_metrics *m);
void ui_show_overtemp(int temp_c);
void ui_clear_overtemp(void);

void ui_set_dialog(const char *text);
void ui_append_dialog(const char *text);
void ui_set_ctx_tps(int ctx, int max_ctx, float tps);
void ui_set_model_status(const char *text);

/* Optional hooks installed by main (may be NULL). */
typedef void (*ui_action_fn)(void);
void ui_set_actions(ui_action_fn on_compress, ui_action_fn on_ask);

#endif
