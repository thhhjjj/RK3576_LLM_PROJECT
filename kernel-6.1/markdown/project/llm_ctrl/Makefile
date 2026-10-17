# SPDX-License-Identifier: GPL-2.0
#
# External module: llm_ctrl.ko (+ optional nl_watch)
#
# On Ubuntu SDK (kernel already built for the board):
#   make KDIR=/path/to/kernel-6.1 CROSS_COMPILE=aarch64-linux-gnu-
#   adb push llm_ctrl.ko /tmp/
#   adb shell 'insmod /tmp/llm_ctrl.ko'
#
# Sysfs:
#   cat /sys/devices/platform/llm_ctrl/temp_c
#   echo 20 > /sys/devices/platform/llm_ctrl/threshold_c   # force overtemp once

ARCH          ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
KDIR          ?= $(abspath $(CURDIR)/../../..)

obj-m += llm_ctrl.o
ccflags-y += -I$(src)

all: modules nl_watch

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

nl_watch: nl_watch.c llm_ctrl.h
	$(CROSS_COMPILE)gcc -Wall -O2 -static -I. -o $@ nl_watch.c

# Allow: make modules   (skip userspace if only testing .ko)
#
# Live board (from Mac, after make modules):
#   adb push llm_ctrl.ko /lib/modules/
#   adb push .../S39llm_ctrl /etc/init.d/S39llm_ctrl
#   adb shell 'chmod +x /etc/init.d/S39llm_ctrl; sync; reboot'
#
# Next rootfs rebuild: leave llm_ctrl.ko here; overlay 30-llm_ctrl/prepare.sh
# copies it into /lib/modules/ automatically.

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -f nl_watch

.PHONY: all modules clean
