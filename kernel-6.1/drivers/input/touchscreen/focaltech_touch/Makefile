# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the focaltech touchscreen drivers.
#

# Each configuration option enables a list of files.

obj-$(CONFIG_TOUCHSCREEN_FTS) += focaltech-ts.o
focaltech-ts-y += focaltech_core.o
focaltech-ts-y += focaltech_ex_fun.o
focaltech-ts-y += focaltech_ex_mode.o
focaltech-ts-y += focaltech_flash.o
focaltech-ts-y += focaltech_gesture.o
focaltech-ts-y += focaltech_esdcheck.o
focaltech-ts-y += focaltech_i2c.o
focaltech-ts-y += focaltech_point_report_check.o
focaltech-ts-y += focaltech_upgrade_ft8201.o
# Do not use "focaltech_test/" in a composite *-y list: kbuild 6.1 puts the
# directory into vmlinux.a ("member ... is not an object"). Factory test is
# off (FTS_TEST_EN=0); list .o files explicitly if you re-enable it.
# focaltech-ts-y += focaltech_test/focaltech_test.o
# focaltech-ts-y += focaltech_test/focaltech_test_ini.o
# focaltech-ts-y += focaltech_test/supported_ic/focaltech_test_ft8201.o

