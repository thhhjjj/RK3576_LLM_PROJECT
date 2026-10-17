/* SPDX-License-Identifier: MIT */
/* Adapted from app/lvgl_demo/hal/evdev.c — auto-pick *ts* input node. */

#include "evdev.h"

#if USE_EVDEV

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libevdev-1.0/libevdev/libevdev.h>

static struct libevdev *evdev;
static int evdev_fd = -1;
static int evdev_root_x;
static int evdev_root_y;
static int evdev_button;
static int evdev_min_x = DEFAULT_EVDEV_HOR_MIN;
static int evdev_max_x = DEFAULT_EVDEV_HOR_MAX;
static int evdev_min_y = DEFAULT_EVDEV_VER_MIN;
static int evdev_max_y = DEFAULT_EVDEV_VER_MAX;
static int evdev_calibrate;
static int evdev_key_val;
static int evdev_rot;

#define TP_NAME_LEN 32
static char tp_event[TP_NAME_LEN] = EVDEV_NAME;

static int map_range(int x, int in_min, int in_max, int out_min, int out_max)
{
	if (in_max == in_min)
		return out_min;
	return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

static int evdev_get_tp_event(void)
{
	int fd, len, i, input_dev_num = 0;
	DIR *pDir;
	struct dirent *ent;
	char file_name[TP_NAME_LEN];
	char tp_name[TP_NAME_LEN];
	const char *path = "/sys/class/input";

	pDir = opendir(path);
	if (!pDir) {
		printf("%s: open %s failed\n", __func__, path);
		return -1;
	}
	while ((ent = readdir(pDir)) != NULL) {
		if (strstr(ent->d_name, "input"))
			input_dev_num++;
	}
	closedir(pDir);

	for (i = 0; i < input_dev_num; i++) {
		snprintf(file_name, sizeof(file_name),
			 "/sys/class/input/input%d/name", i);
		fd = open(file_name, O_RDONLY);
		if (fd < 0)
			continue;
		len = read(fd, tp_name, TP_NAME_LEN);
		close(fd);
		if (len <= 0)
			continue;
		if (len >= TP_NAME_LEN)
			len = TP_NAME_LEN - 1;
		tp_name[len] = '\0';
		if (strstr(tp_name, "ts")) {
			snprintf(tp_event, sizeof(tp_event),
				 "/dev/input/event%d", i);
			printf("%s: %s -> %s\n", __func__, tp_name, tp_event);
			return 0;
		}
	}
	return -1;
}

int evdev_set_file(lv_disp_drv_t *drv, char *dev_name)
{
	int rc;

	if (evdev_fd != -1)
		close(evdev_fd);
	evdev_fd = -1;
	if (evdev)
		libevdev_free(evdev);
	evdev = NULL;

	evdev_fd = open(dev_name, O_RDWR | O_NOCTTY | O_NDELAY);
	if (evdev_fd == -1) {
		perror("unable open evdev interface");
		return -1;
	}

	rc = libevdev_new_from_fd(evdev_fd, &evdev);
	if (rc < 0) {
		printf("Failed to init libevdev (%s)\n", strerror(-rc));
	} else if (libevdev_has_event_type(evdev, EV_ABS)) {
		const struct input_absinfo *abs;

		if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_X)) {
			abs = libevdev_get_abs_info(evdev, ABS_MT_POSITION_X);
			evdev_min_x = abs->minimum;
			evdev_max_x = abs->maximum;
		}
		if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_Y)) {
			abs = libevdev_get_abs_info(evdev, ABS_MT_POSITION_Y);
			evdev_min_y = abs->minimum;
			evdev_max_y = abs->maximum;
		}
	}

	fcntl(evdev_fd, F_SETFL, O_ASYNC | O_NONBLOCK);
	evdev_root_x = 0;
	evdev_root_y = 0;
	evdev_key_val = 0;
	evdev_button = LV_INDEV_STATE_REL;

	if (evdev_min_x != 0 || evdev_max_x != drv->hor_res ||
	    evdev_min_y != 0 || evdev_max_y != drv->ver_res)
		evdev_calibrate = 1;

	printf("evdev_calibrate = %d (%s)\n", evdev_calibrate, dev_name);
	return 0;
}

int evdev_init(lv_disp_drv_t *drv, int rot)
{
	evdev_rot = rot;
	if (evdev_get_tp_event() < 0) {
		printf("%s: get tp event failed, try %s\n", __func__, EVDEV_NAME);
		return evdev_set_file(drv, EVDEV_NAME);
	}
	return evdev_set_file(drv, tp_event);
}

void evdev_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
	struct input_event in;
	int tmp;

	if (evdev_fd < 0)
		return;

	while (read(evdev_fd, &in, sizeof(in)) > 0) {
		if (in.type == EV_REL) {
			if (in.code == REL_X)
				evdev_root_x += in.value;
			else if (in.code == REL_Y)
				evdev_root_y += in.value;
		} else if (in.type == EV_ABS) {
			if (in.code == ABS_X || in.code == ABS_MT_POSITION_X)
				evdev_root_x = in.value;
			else if (in.code == ABS_Y || in.code == ABS_MT_POSITION_Y)
				evdev_root_y = in.value;
			else if (in.code == ABS_MT_TRACKING_ID) {
				if (in.value == -1)
					evdev_button = LV_INDEV_STATE_REL;
				else
					evdev_button = LV_INDEV_STATE_PR;
			}
		} else if (in.type == EV_KEY) {
			if (in.code == BTN_MOUSE || in.code == BTN_TOUCH)
				evdev_button = in.value ? LV_INDEV_STATE_PR
							: LV_INDEV_STATE_REL;
		}
	}

	if (drv->type != LV_INDEV_TYPE_POINTER)
		return;

	if (evdev_calibrate) {
		data->point.x = map_range(evdev_root_x, evdev_min_x, evdev_max_x,
					  0, drv->disp->driver->hor_res);
		data->point.y = map_range(evdev_root_y, evdev_min_y, evdev_max_y,
					  0, drv->disp->driver->ver_res);
	} else {
		data->point.x = evdev_root_x;
		data->point.y = evdev_root_y;
	}
	data->state = evdev_button;

	if (evdev_rot == 90) {
		tmp = data->point.x;
		data->point.x = data->point.y;
		data->point.y = drv->disp->driver->ver_res - tmp;
	} else if (evdev_rot == 180) {
		tmp = data->point.x;
		data->point.x = drv->disp->driver->hor_res - data->point.y;
		data->point.y = drv->disp->driver->ver_res - tmp;
	} else if (evdev_rot == 270) {
		tmp = data->point.x;
		data->point.x = drv->disp->driver->hor_res - data->point.y;
		data->point.y = tmp;
	}

	if (data->point.x < 0)
		data->point.x = 0;
	if (data->point.y < 0)
		data->point.y = 0;
	if (data->point.x >= drv->disp->driver->hor_res)
		data->point.x = drv->disp->driver->hor_res - 1;
	if (data->point.y >= drv->disp->driver->ver_res)
		data->point.y = drv->disp->driver->ver_res - 1;
}

#endif /* USE_EVDEV */
