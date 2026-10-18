/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_RKLLM_SESSION_H
#define LLM_PANEL_RKLLM_SESSION_H

/*
 * PHASE2 session wrapper.
 * Uses dlopen("librkllmrt.so") so llm_panel still builds without the .so in
 * the Buildroot staging sysroot. Load before LVGL (see main.c).
 */

#define LLM_PANEL_DEFAULT_LIB "/usr/lib/librkllmrt.so"
#define LLM_PANEL_DEFAULT_MODEL "/usr/share/rkllm/QWEN3.5_2B.rkllm"
/* Qwen3.5-2B VLM vision encoder (installed by prepare.sh; UI multimodal later). */
#define LLM_PANEL_DEFAULT_VISION "/usr/share/rkllm/vision.rknn"
#define LLM_PANEL_MAX_CONTEXT 1024
#define LLM_PANEL_COMPRESS_WATER_PCT 70

struct session_stats {
	int loaded;
	int running;
	int ctx_tokens; /* best-effort from kv cache; -1 if unknown */
	int max_context;
	float token_per_s; /* last completed round */
};

int session_init(void);
void session_fini(void);

/* Load model (blocking). path NULL → LLM_PANEL_DEFAULT_MODEL.
 * Returns 0 on success; on failure UI may still start. */
int session_load(const char *model_path);

int session_loaded(void);

/* Ask (async worker). Returns 0 if queued, -1 if busy/unavailable. */
int session_ask(const char *user_text);

/* Abort current generation if any. */
void session_abort(void);

/* Manual/auto compress: keep system, drop oldest turns, destroy+rebuild.
 * Returns 0 on success. */
int session_compress(void);

void session_get_stats(struct session_stats *out);

/* Prefixed smoke question for bring-up without IME. */
const char *session_smoke_question(void);

#endif
