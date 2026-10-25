/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_NPU_DVFS_H
#define LLM_PANEL_NPU_DVFS_H

/*
 * PHASE7-A：推理前后抬/降 NPU 频率（devfreq）。
 * 禁止在 RKLLM 回调里调用。失败只打日志，不挡推理。
 *
 * 关闭：LLM_PANEL_NPU_BOOST=0
 */
void npu_dvfs_init(void);
void npu_dvfs_boost(void);   /* 可嵌套；切 performance → 最高频 */
void npu_dvfs_restore(void); /* 配对 boost；回到进入前的 governor */

#endif
