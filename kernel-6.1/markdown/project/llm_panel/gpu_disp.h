/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_GPU_DISP_H
#define LLM_PANEL_GPU_DISP_H

/*
 * PHASE6 (+ PHASE7-E 提前): LVGL → Mali GBM/EGL → DRM，目标约 60fps。
 * @return 0 on success (registers LVGL disp); -1 → caller uses drm_disp_drv_init.
 *
 * Force dumb: LLM_PANEL_FORCE_DUMB=1
 */
int gpu_disp_init(void);
void gpu_disp_deinit(void);
int gpu_disp_active(void);

/* Present/s over last ~1s window; 0 if GPU path inactive. */
float gpu_disp_fps(void);

/* LVGL refresh timer → 16ms (~60Hz). Safe after any disp register. */
void gpu_disp_set_refr_60hz(void);

#endif
