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
/* 与现成 .rkllm 导出上限一致（社区包 max_context_limit: 4096） */
#define LLM_PANEL_MAX_CONTEXT 4096
#define LLM_PANEL_COMPRESS_WATER_PCT 70

struct session_stats {
	int loaded;			//是否加载
	int running;		//是否正在生成token(查rkllm_is_running句柄，NULL/API不存在，则认为不在生成)
	int ctx_tokens; 	//当前上下文大约用了多少 token（来自 KV 查询的 last_ctx；未知时可能是 -1）
	int max_context;	//最大上下文大小
	float token_per_s; 	//上一轮已完成的生成速度（不是实时瞬时值）
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

/* 1 = 生成本轮尚未 FINISH（abort 后仍为 1，直到回调结束）. */
int session_gen_busy(void);

/*
 * PHASE7-C：异步压缩（投递 worker，不堵 LVGL）。
 * 人设 hist[0] 不动；旧轮 → 摘要条 role'm'；保留近轮；destroy+prefill。
 * @return 0 已排队；-1 忙/未加载。
 */
int session_compress(void);

void session_get_stats(struct session_stats *out);

/* Prefixed smoke question for bring-up without IME. */
const char *session_smoke_question(void);

#endif
