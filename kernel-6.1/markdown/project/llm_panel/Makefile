# SPDX-License-Identifier: GPL-2.0
#
# Host/VM quick test without LVGL:
#   make metrics_test
#
# Full UI is built by Buildroot package llm_panel during ./build.sh rootfs.

ARCH          ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
CC            := $(CROSS_COMPILE)gcc

CFLAGS  ?= -Wall -O2 -static
SRCS_METRICS := metrics_main.c metrics.c

all: metrics_test

metrics_test: $(SRCS_METRICS) metrics.h
	$(CC) $(CFLAGS) -o $@ $(SRCS_METRICS)

clean:
	rm -f metrics_test

.PHONY: all clean
