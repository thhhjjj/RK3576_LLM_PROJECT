cmake_minimum_required(VERSION 3.8)
project(llm_panel C)

add_definitions(-DUSE_DRM=1 -DUSE_EVDEV=1)

include_directories(
	${CMAKE_SYSROOT}/usr/include
	${CMAKE_SYSROOT}/usr/include/libdrm
	${CMAKE_SYSROOT}/usr/include/lvgl
	${CMAKE_SYSROOT}/usr/include/lvgl/lv_drivers
	${PROJECT_SOURCE_DIR}
)

add_executable(llm_panel
	main.c
	ui.c
	metrics.c
	evdev.c
	font.c
	tokq.c
	rkllm_session.c
	ws_hub.c
	cloud_sse.c
	gpu_probe.c
	gpu_disp.c
	npu_dvfs.c
	backlight.c
)

target_link_libraries(llm_panel
	lvgl
	lv_drivers
	freetype
	drm
	evdev
	websockets
	curl
	pthread
	dl
	m
)

install(TARGETS llm_panel RUNTIME DESTINATION bin)
