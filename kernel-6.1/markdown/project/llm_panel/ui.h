/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_UI_H
#define LLM_PANEL_UI_H

#include "metrics.h"

void ui_create(void);
void ui_update_metrics(const struct llm_metrics *m);
void ui_show_overtemp(int temp_c);
void ui_clear_overtemp(void);

#endif
