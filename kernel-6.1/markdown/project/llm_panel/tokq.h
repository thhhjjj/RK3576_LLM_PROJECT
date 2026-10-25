/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_TOKQ_H
#define LLM_PANEL_TOKQ_H

/*
 * Bounded queue + fan-out bus (PHASE3).
 * Producers (RKLLM / SSE) only push — never draw LVGL or lws_write here.
 * UI drains via tokq_pop; optional fanout_fn gets a copy (e.g. WS queues).
 */

enum tokq_kind {
	TOKQ_TOKEN = 0, /* streaming token chunk */
	TOKQ_FINISH,    /* end of turn */
	TOKQ_ERROR,     /* error; ends turn */
	TOKQ_STATUS,    /* status line in text */
};

struct tokq_evt {
	enum tokq_kind kind;
	char text[256];
};

typedef void (*tokq_fanout_fn)(const struct tokq_evt *ev);

int tokq_init(void);
void tokq_fini(void);

/* Fan-out after each push (may run on RKLLM callback thread — must be nonblocking). */
void tokq_set_fanout(tokq_fanout_fn fn);

/* Returns 0 on UI-queue push, -1 if UI queue full (fan-out still runs). */
int tokq_push(enum tokq_kind kind, const char *text);

/* Returns 1 if popped, 0 if empty. */
int tokq_pop(struct tokq_evt *out);

#endif
