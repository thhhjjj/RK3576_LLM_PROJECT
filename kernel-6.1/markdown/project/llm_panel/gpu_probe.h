/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_GPU_PROBE_H
#define LLM_PANEL_GPU_PROBE_H

/*
 * PHASE6: optional Mali/EGL bring-up check (dlopen, no build-time EGL headers).
 * Does not replace DRM dumb LVGL yet — only proves userspace GPU stack loads.
 *
 * @return 0 if EGL+GBM context created (then destroyed); -1 on failure
 */
int gpu_probe_egl(void);

#endif
