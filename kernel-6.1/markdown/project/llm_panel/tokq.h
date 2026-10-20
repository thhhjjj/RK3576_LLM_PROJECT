/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_TOKQ_H
#define LLM_PANEL_TOKQ_H

/*
 * Bounded queue: RKLLM callback / worker → LVGL thread.
 * Callback must only push; UI drains via tokq_pop.
 */

enum tokq_kind {
	TOKQ_TOKEN = 0,
	TOKQ_FINISH,
	TOKQ_ERROR,
	TOKQ_STATUS, /* human-readable status line for dialog */
};

struct tokq_evt {//事件队列元素
	enum tokq_kind kind; //事件类型
	char text[256]; //事件文本
};

int tokq_init(void);
void tokq_fini(void);

/* Returns 0 on push, -1 if full (drops; never blocks). */
int tokq_push(enum tokq_kind kind, const char *text);

/* Returns 1 if popped, 0 if empty. */
int tokq_pop(struct tokq_evt *out);

#endif
