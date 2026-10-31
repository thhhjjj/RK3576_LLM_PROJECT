/* SPDX-License-Identifier: MIT */
/*
 * 板端 liblvgl 常关 LV_USE_IME_PINYIN；在本包内强制编入官方拼音 IME。
 * 若系统 LVGL 已打开 IME，本文件为空，避免重复符号。
 */
#include "lvgl/lvgl.h"

#if LV_USE_IME_PINYIN

/* 系统已提供拼音 IME，无需捆绑 */

#else

#undef LV_USE_IME_PINYIN
#define LV_USE_IME_PINYIN 1

#ifndef LV_IME_PINYIN_USE_DEFAULT_DICT
#define LV_IME_PINYIN_USE_DEFAULT_DICT 1
#endif
#ifndef LV_IME_PINYIN_CAND_TEXT_NUM
#define LV_IME_PINYIN_CAND_TEXT_NUM 6
#endif
#ifndef LV_IME_PINYIN_USE_K9_MODE
#define LV_IME_PINYIN_USE_K9_MODE 1
#endif
#ifndef LV_IME_PINYIN_K9_CAND_TEXT_NUM
#define LV_IME_PINYIN_K9_CAND_TEXT_NUM 3
#endif

/* 先用独立头拉齐类型，再吃进官方 .c */
#include "llm_lv_ime_pinyin.h"
#include "lv_ime_pinyin.c"

#endif /* !LV_USE_IME_PINYIN */
