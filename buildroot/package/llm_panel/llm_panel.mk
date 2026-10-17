################################################################################
#
# llm_panel — PHASE1 LVGL UI
#
################################################################################

LLM_PANEL_SITE = $(TOPDIR)/../kernel-6.1/markdown/project/llm_panel
LLM_PANEL_SITE_METHOD = local
# Package name is lv_drivers (underscore), same as rkpartybox.mk — not lv-drivers.
LLM_PANEL_DEPENDENCIES = lvgl lv_drivers freetype libdrm libevdev

$(eval $(cmake-package))
