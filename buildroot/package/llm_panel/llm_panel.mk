################################################################################
#
# llm_panel — PHASE1 LVGL UI
#
################################################################################

LLM_PANEL_SITE = $(TOPDIR)/../kernel-6.1/markdown/project/llm_panel
LLM_PANEL_SITE_METHOD = local
# Avoid in-tree clash with any leftover Makefile (was building metrics_test only).
LLM_PANEL_SUPPORTS_IN_SOURCE_BUILD = NO
# Package name is lv_drivers (underscore), same as rkpartybox.mk — not lv-drivers.
LLM_PANEL_DEPENDENCIES = lvgl lv_drivers freetype libdrm libevdev libwebsockets libcurl

# Adobe Source Han Sans CN SubsetOTF Regular (~8MB) — enough for UI + dialogue CJK.
define LLM_PANEL_INSTALL_FONT
	$(INSTALL) -D -m 0644 $(@D)/fonts/SourceHanSansCN-Regular.otf \
		$(TARGET_DIR)/usr/share/fonts/source-han-sans-cn/SourceHanSansCN-Regular.otf
endef
LLM_PANEL_POST_INSTALL_TARGET_HOOKS += LLM_PANEL_INSTALL_FONT

$(eval $(cmake-package))
