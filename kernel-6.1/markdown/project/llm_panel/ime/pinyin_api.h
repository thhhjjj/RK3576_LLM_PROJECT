/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_PINYIN_API_H
#define LLM_PANEL_PINYIN_API_H

#include "lvgl/lvgl.h"

/*
 * 系统 LVGL 常关 IME：强制打开宏后再暴露捆绑实现的 API。
 * 系统已开 IME：同样走我们的头（声明一致），符号由 liblvgl 提供。
 */
#if !LV_USE_IME_PINYIN
#undef LV_USE_IME_PINYIN
#define LV_USE_IME_PINYIN 1
#ifndef LV_IME_PINYIN_USE_DEFAULT_DICT
#define LV_IME_PINYIN_USE_DEFAULT_DICT 1
#endif
#ifndef LV_IME_PINYIN_CAND_TEXT_NUM
#define LV_IME_PINYIN_CAND_TEXT_NUM 6
#endif
#ifndef LV_IME_PINYIN_USE_K9_MODE
/* 九键切布局在板子上易崩；默认只用 26 键拼音 */
#define LV_IME_PINYIN_USE_K9_MODE 0
#endif
#ifndef LV_IME_PINYIN_K9_CAND_TEXT_NUM
#define LV_IME_PINYIN_K9_CAND_TEXT_NUM 3
#endif
#endif

#include "llm_lv_ime_pinyin.h"

#endif /* LLM_PANEL_PINYIN_API_H */
