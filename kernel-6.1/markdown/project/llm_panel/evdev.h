/* SPDX-License-Identifier: MIT */
/* Rockchip-style touch via libevdev (lv_drivers package does not build USE_EVDEV). */
#ifndef LLM_PANEL_EVDEV_H
#define LLM_PANEL_EVDEV_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef USE_EVDEV
#define USE_EVDEV 1
#endif

#if USE_EVDEV
#include "lvgl/lvgl.h"

#define EVDEV_NAME "/dev/input/event0"
#define EVDEV_SWAP_AXES 0
#define DEFAULT_EVDEV_HOR_MIN 0
#define DEFAULT_EVDEV_HOR_MAX 720
#define DEFAULT_EVDEV_VER_MIN 0
#define DEFAULT_EVDEV_VER_MAX 1280

int evdev_init(lv_disp_drv_t *drv, int rot);
int evdev_set_file(lv_disp_drv_t *drv, char *dev_name);
void evdev_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
#endif

#ifdef __cplusplus
}
#endif

#endif
