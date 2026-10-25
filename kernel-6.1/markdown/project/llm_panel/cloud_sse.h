/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_CLOUD_SSE_H
#define LLM_PANEL_CLOUD_SSE_H

/*
 * PHASE3: DeepSeek (OpenAI-compatible) streaming via libcurl SSE.
 * API Key: process memory only — set via env or cloud_sse_set_key(); never log.
 *
 * Default:
 *   base  https://api.deepseek.com
 *   model deepseek-chat
 * Env bring-up: DEEPSEEK_API_KEY or LLM_PANEL_API_KEY
 */

int cloud_sse_init(void);
void cloud_sse_fini(void);

/* Copy key into memory; empty/NULL clears. Never written to disk/log. */
void cloud_sse_set_key(const char *key);
int cloud_sse_has_key(void);

/* Load key from env if not already set. Safe to call once at startup. */
void cloud_sse_load_key_from_env(void);

/* Async ask → tokq TOKEN/FINISH/ERROR. Returns 0 queued, -1 busy/no key. */
int cloud_sse_ask(const char *user_text);

/* Abort in-flight HTTP if any. */
void cloud_sse_abort(void);

int cloud_sse_busy(void);

#endif
