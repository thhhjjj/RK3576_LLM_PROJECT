/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE6 GPU display: LVGL SW → GLES texture → GBM/EGL swap → DRM CRTC.
 * All GBM/EGL/GLES via dlopen (llm staging may lack mali headers).
 */

#include "gpu_disp.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "lvgl/lvgl.h"

/* ---- Khronos / GBM constants (subset) ---- */
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_DISPLAY ((void *)0)
#define EGL_NO_CONTEXT ((void *)0)
#define EGL_NO_SURFACE ((void *)0)
#define EGL_SUCCESS 0x3000
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_NONE 0x3038
#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_PLATFORM_GBM_KHR 0x31D7

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_BGRA_EXT 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TRIANGLES 0x0004
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_FALSE 0

#define GBM_FORMAT_XRGB8888 0x34325258
#define GBM_BO_USE_SCANOUT (1 << 0)
#define GBM_BO_USE_RENDERING (1 << 2)

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef void *EGLNativeWindowType;
typedef unsigned int EGLenum;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef int GLsizei;

struct gbm_device;
struct gbm_surface;
struct gbm_bo;
union gbm_bo_handle {
	void *ptr;
	uint32_t u32;
	uint64_t u64;
	int32_t s32;
	int64_t s64;
};

/* ---- dlsym tables ---- */
static void *libgbm, *libegl, *libgles;

static struct gbm_device *(*p_gbm_create_device)(int);
static void (*p_gbm_device_destroy)(struct gbm_device *);
static struct gbm_surface *(*p_gbm_surface_create)(struct gbm_device *, uint32_t,
						   uint32_t, uint32_t, uint32_t);
static void (*p_gbm_surface_destroy)(struct gbm_surface *);
static struct gbm_bo *(*p_gbm_surface_lock_front_buffer)(struct gbm_surface *);
static void (*p_gbm_surface_release_buffer)(struct gbm_surface *,
					    struct gbm_bo *);
static union gbm_bo_handle (*p_gbm_bo_get_handle)(struct gbm_bo *);
static uint32_t (*p_gbm_bo_get_stride)(struct gbm_bo *);
static uint32_t (*p_gbm_bo_get_width)(struct gbm_bo *);
static uint32_t (*p_gbm_bo_get_height)(struct gbm_bo *);

static EGLDisplay (*p_eglGetPlatformDisplay)(EGLenum, void *, const EGLint *);
static EGLDisplay (*p_eglGetDisplay)(void *);
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*p_eglBindAPI)(EGLenum);
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
				       EGLint, EGLint *);
static EGLBoolean (*p_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint,
					  EGLint *);
static EGLSurface (*p_eglCreateWindowSurface)(EGLDisplay, EGLConfig,
					      EGLNativeWindowType,
					      const EGLint *);
static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
					const EGLint *);
static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
				      EGLContext);
static EGLBoolean (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean (*p_eglSwapInterval)(EGLDisplay, EGLint);
static EGLBoolean (*p_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*p_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLBoolean (*p_eglTerminate)(EGLDisplay);
static void *(*p_eglGetProcAddress)(const char *);
static EGLint (*p_eglGetError)(void);

static void (*p_glClear)(GLenum);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glGenTextures)(GLsizei, GLuint *);
static void (*p_glBindTexture)(GLenum, GLuint);
static void (*p_glTexParameteri)(GLenum, GLenum, GLint);
static void (*p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
			      GLenum, GLenum, const void *);
static void (*p_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei,
				 GLenum, GLenum, const void *);
static void (*p_glActiveTexture)(GLenum);
static GLuint (*p_glCreateShader)(GLenum);
static void (*p_glShaderSource)(GLuint, GLsizei, const GLchar *const *,
				const GLint *);
static void (*p_glCompileShader)(GLuint);
static void (*p_glGetShaderiv)(GLuint, GLenum, GLint *);
static GLuint (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(GLuint, GLuint);
static void (*p_glLinkProgram)(GLuint);
static void (*p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void (*p_glUseProgram)(GLuint);
static GLint (*p_glGetAttribLocation)(GLuint, const GLchar *);
static GLint (*p_glGetUniformLocation)(GLuint, const GLchar *);
static void (*p_glUniform1i)(GLint, GLint);
static void (*p_glGenBuffers)(GLsizei, GLuint *);
static void (*p_glBindBuffer)(GLenum, GLuint);
static void (*p_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*p_glEnableVertexAttribArray)(GLuint);
static void (*p_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean,
				       GLsizei, const void *);
static void (*p_glDrawArrays)(GLenum, GLint, GLsizei);
static void (*p_glDeleteShader)(GLuint);
static GLenum (*p_glGetError)(void);

/* ---- runtime state ---- */
static int g_active;
static int g_drm_fd = -1;
static drmModeRes *g_res;
static drmModeConnector *g_conn;
static drmModeCrtc *g_crtc_saved;
static uint32_t g_conn_id, g_crtc_id;
static drmModeModeInfo g_mode;
static int g_w, g_h;

static struct gbm_device *g_gbm;
static struct gbm_surface *g_gsurf;
static struct gbm_bo *g_bo_prev;
static uint32_t g_fb_prev;
static int g_crtc_on;
static int g_flip_waiting;

static unsigned g_frames_window;
static struct timespec g_fps_t0;
static float g_fps;

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_surf = EGL_NO_SURFACE;
static GLuint g_tex, g_prog, g_vbo;
static GLint g_a_pos, g_a_uv, g_u_tex;
static GLenum g_tex_fmt = GL_RGBA;

static lv_color_t *g_buf1, *g_buf2;
static lv_disp_draw_buf_t g_draw_buf;
static lv_disp_drv_t g_disp_drv;

static int load_syms(void)
{
	libgbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!libgbm)
		libgbm = dlopen("libgbm.so", RTLD_NOW | RTLD_GLOBAL);
	libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!libegl)
		libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
	dlopen("libwayland-server.so.0", RTLD_NOW | RTLD_GLOBAL);
	dlopen("libmali.so.1", RTLD_NOW | RTLD_GLOBAL);
	libgles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
	if (!libgles)
		libgles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);

	if (!libgbm || !libegl) {
		fprintf(stderr, "gpu_disp: dlopen gbm/EGL: %s\n", dlerror());
		return -1;
	}

#define S(h, p, n)                                                             \
	do {                                                                   \
		p = dlsym(h, n);                                               \
		if (!p) {                                                      \
			fprintf(stderr, "gpu_disp: missing %s\n", n);          \
			return -1;                                             \
		}                                                              \
	} while (0)

	S(libgbm, p_gbm_create_device, "gbm_create_device");
	S(libgbm, p_gbm_device_destroy, "gbm_device_destroy");
	S(libgbm, p_gbm_surface_create, "gbm_surface_create");
	S(libgbm, p_gbm_surface_destroy, "gbm_surface_destroy");
	S(libgbm, p_gbm_surface_lock_front_buffer, "gbm_surface_lock_front_buffer");
	S(libgbm, p_gbm_surface_release_buffer, "gbm_surface_release_buffer");
	S(libgbm, p_gbm_bo_get_handle, "gbm_bo_get_handle");
	S(libgbm, p_gbm_bo_get_stride, "gbm_bo_get_stride");
	S(libgbm, p_gbm_bo_get_width, "gbm_bo_get_width");
	S(libgbm, p_gbm_bo_get_height, "gbm_bo_get_height");

	p_eglGetPlatformDisplay = dlsym(libegl, "eglGetPlatformDisplay");
	if (!p_eglGetPlatformDisplay)
		p_eglGetPlatformDisplay = dlsym(libegl, "eglGetPlatformDisplayEXT");
	S(libegl, p_eglGetDisplay, "eglGetDisplay");
	S(libegl, p_eglInitialize, "eglInitialize");
	S(libegl, p_eglBindAPI, "eglBindAPI");
	S(libegl, p_eglChooseConfig, "eglChooseConfig");
	S(libegl, p_eglGetConfigAttrib, "eglGetConfigAttrib");
	S(libegl, p_eglCreateWindowSurface, "eglCreateWindowSurface");
	S(libegl, p_eglCreateContext, "eglCreateContext");
	S(libegl, p_eglMakeCurrent, "eglMakeCurrent");
	S(libegl, p_eglSwapBuffers, "eglSwapBuffers");
	p_eglSwapInterval = dlsym(libegl, "eglSwapInterval");
	S(libegl, p_eglDestroySurface, "eglDestroySurface");
	S(libegl, p_eglDestroyContext, "eglDestroyContext");
	S(libegl, p_eglTerminate, "eglTerminate");
	S(libegl, p_eglGetProcAddress, "eglGetProcAddress");
	S(libegl, p_eglGetError, "eglGetError");

#undef S

	/* GLES: prefer eglGetProcAddress, fall back to libGLESv2 */
#define G(p, n)                                                                \
	do {                                                                   \
		p = (void *)p_eglGetProcAddress(n);                            \
		if (!p && libgles)                                             \
			p = dlsym(libgles, n);                                 \
		if (!p) {                                                      \
			fprintf(stderr, "gpu_disp: missing GL %s\n", n);       \
			return -1;                                             \
		}                                                              \
	} while (0)

	G(p_glClear, "glClear");
	G(p_glClearColor, "glClearColor");
	G(p_glViewport, "glViewport");
	G(p_glGenTextures, "glGenTextures");
	G(p_glBindTexture, "glBindTexture");
	G(p_glTexParameteri, "glTexParameteri");
	G(p_glTexImage2D, "glTexImage2D");
	G(p_glTexSubImage2D, "glTexSubImage2D");
	G(p_glActiveTexture, "glActiveTexture");
	G(p_glCreateShader, "glCreateShader");
	G(p_glShaderSource, "glShaderSource");
	G(p_glCompileShader, "glCompileShader");
	G(p_glGetShaderiv, "glGetShaderiv");
	G(p_glCreateProgram, "glCreateProgram");
	G(p_glAttachShader, "glAttachShader");
	G(p_glLinkProgram, "glLinkProgram");
	G(p_glGetProgramiv, "glGetProgramiv");
	G(p_glUseProgram, "glUseProgram");
	G(p_glGetAttribLocation, "glGetAttribLocation");
	G(p_glGetUniformLocation, "glGetUniformLocation");
	G(p_glUniform1i, "glUniform1i");
	G(p_glGenBuffers, "glGenBuffers");
	G(p_glBindBuffer, "glBindBuffer");
	G(p_glBufferData, "glBufferData");
	G(p_glEnableVertexAttribArray, "glEnableVertexAttribArray");
	G(p_glVertexAttribPointer, "glVertexAttribPointer");
	G(p_glDrawArrays, "glDrawArrays");
	G(p_glDeleteShader, "glDeleteShader");
	G(p_glGetError, "glGetError");
#undef G
	return 0;
}

static int open_drm_with_connector(void)
{
	const char *nodes[] = { "/dev/dri/card0", "/dev/dri/card1", NULL };
	int i;

	for (i = 0; nodes[i]; i++) {
		int fd = open(nodes[i], O_RDWR | O_CLOEXEC);
		drmModeRes *res;
		int c;

		if (fd < 0)
			continue;
		res = drmModeGetResources(fd);
		if (!res) {
			close(fd);
			continue;
		}
		for (c = 0; c < res->count_connectors; c++) {
			drmModeConnector *conn =
				drmModeGetConnector(fd, res->connectors[c]);
			if (!conn)
				continue;
			if (conn->connection == DRM_MODE_CONNECTED &&
			    conn->count_modes > 0) {
				g_drm_fd = fd;
				g_res = res;
				g_conn = conn;
				g_conn_id = conn->connector_id;
				g_mode = conn->modes[0];
				g_w = g_mode.hdisplay;
				g_h = g_mode.vdisplay;
				fprintf(stderr,
					"gpu_disp: %s connector %u %dx%d\n",
					nodes[i], g_conn_id, g_w, g_h);
				return 0;
			}
			drmModeFreeConnector(conn);
		}
		drmModeFreeResources(res);
		close(fd);
	}
	fprintf(stderr, "gpu_disp: no connected DRM connector\n");
	return -1;
}

static int find_crtc(void)
{
	int e, j;

	if (g_conn->encoder_id) {
		drmModeEncoder *enc =
			drmModeGetEncoder(g_drm_fd, g_conn->encoder_id);
		if (enc) {
			g_crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);
			if (g_crtc_id)
				goto got;
		}
	}
	for (e = 0; e < g_conn->count_encoders; e++) {
		drmModeEncoder *enc =
			drmModeGetEncoder(g_drm_fd, g_conn->encoders[e]);
		if (!enc)
			continue;
		for (j = 0; j < g_res->count_crtcs; j++) {
			if (enc->possible_crtcs & (1u << j)) {
				g_crtc_id = g_res->crtcs[j];
				drmModeFreeEncoder(enc);
				goto got;
			}
		}
		drmModeFreeEncoder(enc);
	}
	fprintf(stderr, "gpu_disp: no CRTC\n");
	return -1;
got:
	g_crtc_saved = drmModeGetCrtc(g_drm_fd, g_crtc_id);
	fprintf(stderr, "gpu_disp: crtc %u\n", g_crtc_id);
	return 0;
}

static GLuint compile_shader(GLenum type, const char *src)
{
	GLuint s = p_glCreateShader(type);
	GLint ok = 0;

	p_glShaderSource(s, 1, &src, NULL);
	p_glCompileShader(s);
	p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "gpu_disp: shader compile failed\n");
		return 0;
	}
	return s;
}

static int setup_gl(void)
{
	static const char *vs =
		"attribute vec2 a_pos;\n"
		"attribute vec2 a_uv;\n"
		"varying vec2 v_uv;\n"
		"void main(){\n"
		"  v_uv = a_uv;\n"
		"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
		"}\n";
	/* LVGL 32bpp is typically B,G,R,A in memory; without BGRA_EXT we
	 * upload as RGBA so swizzle back in the shader. */
	static const char *fs =
		"precision mediump float;\n"
		"uniform sampler2D u_tex;\n"
		"varying vec2 v_uv;\n"
		"void main(){\n"
		"  vec4 c = texture2D(u_tex, v_uv);\n"
		"  gl_FragColor = vec4(c.b, c.g, c.r, c.a);\n"
		"}\n";
	/* pos.xy uv.xy — Y flip for DRM (0,0 top-left in LVGL) */
	static const GLfloat verts[] = {
		-1.f, -1.f, 0.f, 1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f, 0.f, 0.f,
		-1.f, 1.f,  0.f, 0.f, 1.f, -1.f, 1.f, 1.f, 1.f,  1.f, 1.f, 0.f,
	};
	GLuint vsh, fsh;
	GLint ok = 0;

	vsh = compile_shader(GL_VERTEX_SHADER, vs);
	fsh = compile_shader(GL_FRAGMENT_SHADER, fs);
	if (!vsh || !fsh)
		return -1;
	g_prog = p_glCreateProgram();
	p_glAttachShader(g_prog, vsh);
	p_glAttachShader(g_prog, fsh);
	p_glLinkProgram(g_prog);
	p_glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
	p_glDeleteShader(vsh);
	p_glDeleteShader(fsh);
	if (!ok) {
		fprintf(stderr, "gpu_disp: program link failed\n");
		return -1;
	}
	g_a_pos = p_glGetAttribLocation(g_prog, "a_pos");
	g_a_uv = p_glGetAttribLocation(g_prog, "a_uv");
	g_u_tex = p_glGetUniformLocation(g_prog, "u_tex");

	p_glGenBuffers(1, &g_vbo);
	p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	p_glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	p_glGenTextures(1, &g_tex);
	p_glBindTexture(GL_TEXTURE_2D, g_tex);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* Prefer BGRA to match typical LV_COLOR_DEPTH 32 memory order */
	g_tex_fmt = GL_BGRA_EXT;
	p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_w, g_h, 0, g_tex_fmt,
		       GL_UNSIGNED_BYTE, NULL);
	if (p_glGetError() != 0) {
		g_tex_fmt = GL_RGBA;
		p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_w, g_h, 0, g_tex_fmt,
			       GL_UNSIGNED_BYTE, NULL);
		fprintf(stderr, "gpu_disp: tex format RGBA (no BGRA_EXT)\n");
	} else {
		fprintf(stderr, "gpu_disp: tex format BGRA_EXT\n");
	}

	p_glViewport(0, 0, g_w, g_h);
	p_glClearColor(0.f, 0.f, 0.f, 1.f);
	return 0;
}

static void fps_tick(void)
{
	struct timespec now;
	double dt;

	g_frames_window++;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (g_fps_t0.tv_sec == 0 && g_fps_t0.tv_nsec == 0) {
		g_fps_t0 = now;
		return;
	}
	dt = (now.tv_sec - g_fps_t0.tv_sec) +
	     (now.tv_nsec - g_fps_t0.tv_nsec) / 1e9;
	if (dt >= 1.0) {
		g_fps = (float)(g_frames_window / dt);
		g_frames_window = 0;
		g_fps_t0 = now;
	}
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
			      unsigned int usec, void *data)
{
	(void)fd;
	(void)frame;
	(void)sec;
	(void)usec;
	*(int *)data = 0;
}

static int wait_page_flip(void)
{
	drmEventContext ev = {
		.version = 2,
		.page_flip_handler = page_flip_handler,
	};

	while (g_flip_waiting) {
		struct pollfd pfd = { .fd = g_drm_fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 50);

		if (pr < 0 && errno != EINTR)
			return -1;
		if (pr > 0)
			drmHandleEvent(g_drm_fd, &ev);
	}
	return 0;
}

static int present_gbm(void)
{
	struct gbm_bo *bo;
	uint32_t handle, stride, fb = 0;
	int ret;

	bo = p_gbm_surface_lock_front_buffer(g_gsurf);
	if (!bo) {
		fprintf(stderr, "gpu_disp: lock_front_buffer failed\n");
		return -1;
	}
	handle = p_gbm_bo_get_handle(bo).u32;
	stride = p_gbm_bo_get_stride(bo);
	ret = drmModeAddFB(g_drm_fd, p_gbm_bo_get_width(bo),
			   p_gbm_bo_get_height(bo), 24, 32, stride, handle, &fb);
	if (ret) {
		fprintf(stderr, "gpu_disp: AddFB: %s\n", strerror(errno));
		p_gbm_surface_release_buffer(g_gsurf, bo);
		return -1;
	}

	if (!g_crtc_on) {
		ret = drmModeSetCrtc(g_drm_fd, g_crtc_id, fb, 0, 0, &g_conn_id,
				     1, &g_mode);
		if (ret) {
			fprintf(stderr, "gpu_disp: SetCrtc: %s\n",
				strerror(errno));
			drmModeRmFB(g_drm_fd, fb);
			p_gbm_surface_release_buffer(g_gsurf, bo);
			return -1;
		}
		g_crtc_on = 1;
	} else {
		g_flip_waiting = 1;
		ret = drmModePageFlip(g_drm_fd, g_crtc_id, fb,
				      DRM_MODE_PAGE_FLIP_EVENT, &g_flip_waiting);
		if (ret) {
			g_flip_waiting = 0;
			ret = drmModeSetCrtc(g_drm_fd, g_crtc_id, fb, 0, 0,
					     &g_conn_id, 1, &g_mode);
			if (ret) {
				fprintf(stderr, "gpu_disp: flip/SetCrtc: %s\n",
					strerror(errno));
				drmModeRmFB(g_drm_fd, fb);
				p_gbm_surface_release_buffer(g_gsurf, bo);
				return -1;
			}
		} else {
			wait_page_flip();
		}
	}

	if (g_fb_prev)
		drmModeRmFB(g_drm_fd, g_fb_prev);
	if (g_bo_prev)
		p_gbm_surface_release_buffer(g_gsurf, g_bo_prev);
	g_fb_prev = fb;
	g_bo_prev = bo;
	fps_tick();
	return 0;
}

static void gpu_draw_fullscreen(void)
{
	p_glClear(GL_COLOR_BUFFER_BIT);
	p_glUseProgram(g_prog);
	p_glActiveTexture(GL_TEXTURE0);
	p_glBindTexture(GL_TEXTURE_2D, g_tex);
	p_glUniform1i(g_u_tex, 0);
	p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	p_glEnableVertexAttribArray((GLuint)g_a_pos);
	p_glVertexAttribPointer((GLuint)g_a_pos, 2, GL_FLOAT, GL_FALSE,
				4 * sizeof(GLfloat), (void *)0);
	p_glEnableVertexAttribArray((GLuint)g_a_uv);
	p_glVertexAttribPointer((GLuint)g_a_uv, 2, GL_FLOAT, GL_FALSE,
				4 * sizeof(GLfloat),
				(void *)(2 * sizeof(GLfloat)));
	p_glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void gpu_flush(lv_disp_drv_t *drv, const lv_area_t *area,
		      lv_color_t *color_p)
{
	int32_t w, h;

	if (!g_active) {
		lv_disp_flush_ready(drv);
		return;
	}
	w = area->x2 - area->x1 + 1;
	h = area->y2 - area->y1 + 1;

	p_glBindTexture(GL_TEXTURE_2D, g_tex);
	p_glTexSubImage2D(GL_TEXTURE_2D, 0, area->x1, area->y1, w, h, g_tex_fmt,
			  GL_UNSIGNED_BYTE, color_p);

	/* One present per LVGL frame (PHASE7-E ~60fps path). */
	if (lv_disp_flush_is_last(drv)) {
		gpu_draw_fullscreen();
		if (!p_eglSwapBuffers(g_dpy, g_surf))
			fprintf(stderr, "gpu_disp: SwapBuffers err=%d\n",
				p_eglGetError());
		else if (present_gbm() != 0)
			fprintf(stderr, "gpu_disp: present failed\n");
	}

	lv_disp_flush_ready(drv);
}

float gpu_disp_fps(void)
{
	return g_active ? g_fps : 0.f;
}

void gpu_disp_set_refr_60hz(void)
{
	lv_disp_t *d = lv_disp_get_default();

	if (d && d->refr_timer)
		lv_timer_set_period(d->refr_timer, 16);
}

void gpu_disp_deinit(void)
{
	if (g_crtc_saved && g_drm_fd >= 0) {
		drmModeSetCrtc(g_drm_fd, g_crtc_saved->crtc_id,
			       g_crtc_saved->buffer_id, g_crtc_saved->x,
			       g_crtc_saved->y, &g_conn_id, 1,
			       &g_crtc_saved->mode);
		drmModeFreeCrtc(g_crtc_saved);
		g_crtc_saved = NULL;
	}
	if (g_flip_waiting && g_drm_fd >= 0)
		wait_page_flip();
	if (g_fb_prev && g_drm_fd >= 0) {
		drmModeRmFB(g_drm_fd, g_fb_prev);
		g_fb_prev = 0;
	}
	if (g_bo_prev && g_gsurf) {
		p_gbm_surface_release_buffer(g_gsurf, g_bo_prev);
		g_bo_prev = NULL;
	}
	g_crtc_on = 0;
	g_fps = 0.f;
	g_frames_window = 0;
	memset(&g_fps_t0, 0, sizeof(g_fps_t0));
	if (g_dpy != EGL_NO_DISPLAY) {
		p_eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
				 EGL_NO_CONTEXT);
		if (g_surf != EGL_NO_SURFACE)
			p_eglDestroySurface(g_dpy, g_surf);
		if (g_ctx != EGL_NO_CONTEXT)
			p_eglDestroyContext(g_dpy, g_ctx);
		p_eglTerminate(g_dpy);
	}
	g_dpy = EGL_NO_DISPLAY;
	g_surf = EGL_NO_SURFACE;
	g_ctx = EGL_NO_CONTEXT;
	if (g_gsurf)
		p_gbm_surface_destroy(g_gsurf);
	g_gsurf = NULL;
	if (g_gbm)
		p_gbm_device_destroy(g_gbm);
	g_gbm = NULL;
	if (g_conn)
		drmModeFreeConnector(g_conn);
	g_conn = NULL;
	if (g_res)
		drmModeFreeResources(g_res);
	g_res = NULL;
	if (g_drm_fd >= 0)
		close(g_drm_fd);
	g_drm_fd = -1;
	free(g_buf1);
	free(g_buf2);
	g_buf1 = g_buf2 = NULL;
	g_active = 0;
}

int gpu_disp_active(void)
{
	return g_active;
}

int gpu_disp_init(void)
{
	EGLint major = 0, minor = 0, ncfg = 0;
	EGLConfig cfg;
	EGLint visual_id = 0;
	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLint cfg_attr[20];
	int ai = 0;
	size_t px;

	if (getenv("LLM_PANEL_FORCE_DUMB")) {
		fprintf(stderr, "gpu_disp: FORCE_DUMB set — skip\n");
		return -1;
	}
	if (load_syms() != 0)
		return -1;
	if (open_drm_with_connector() != 0)
		goto fail;
	if (find_crtc() != 0)
		goto fail;

	g_gbm = p_gbm_create_device(g_drm_fd);
	if (!g_gbm) {
		fprintf(stderr, "gpu_disp: gbm_create_device failed\n");
		goto fail;
	}
	g_gsurf = p_gbm_surface_create(g_gbm, (uint32_t)g_w, (uint32_t)g_h,
				       GBM_FORMAT_XRGB8888,
				       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!g_gsurf) {
		fprintf(stderr, "gpu_disp: gbm_surface_create failed\n");
		goto fail;
	}

	if (p_eglGetPlatformDisplay)
		g_dpy = p_eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, g_gbm,
						NULL);
	if (g_dpy == EGL_NO_DISPLAY || !g_dpy)
		g_dpy = p_eglGetDisplay((void *)(intptr_t)g_drm_fd);
	if (g_dpy == EGL_NO_DISPLAY || !g_dpy) {
		fprintf(stderr, "gpu_disp: eglGetDisplay failed\n");
		goto fail;
	}
	if (!p_eglInitialize(g_dpy, &major, &minor)) {
		fprintf(stderr, "gpu_disp: eglInitialize failed err=%d\n",
			p_eglGetError());
		goto fail;
	}
	fprintf(stderr, "gpu_disp: EGL %d.%d\n", major, minor);
	p_eglBindAPI(EGL_OPENGL_ES_API);

	cfg_attr[ai++] = EGL_SURFACE_TYPE;
	cfg_attr[ai++] = EGL_WINDOW_BIT;
	cfg_attr[ai++] = EGL_RENDERABLE_TYPE;
	cfg_attr[ai++] = EGL_OPENGL_ES2_BIT;
	cfg_attr[ai++] = EGL_RED_SIZE;
	cfg_attr[ai++] = 8;
	cfg_attr[ai++] = EGL_GREEN_SIZE;
	cfg_attr[ai++] = 8;
	cfg_attr[ai++] = EGL_BLUE_SIZE;
	cfg_attr[ai++] = 8;
	cfg_attr[ai++] = EGL_ALPHA_SIZE;
	cfg_attr[ai++] = 0;
	cfg_attr[ai++] = EGL_NONE;

	if (!p_eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
		fprintf(stderr, "gpu_disp: eglChooseConfig failed\n");
		goto fail;
	}
	p_eglGetConfigAttrib(g_dpy, cfg, EGL_NATIVE_VISUAL_ID, &visual_id);
	(void)visual_id;

	g_surf = p_eglCreateWindowSurface(g_dpy, cfg, (EGLNativeWindowType)g_gsurf,
					  NULL);
	if (g_surf == EGL_NO_SURFACE || !g_surf) {
		fprintf(stderr, "gpu_disp: CreateWindowSurface err=%d\n",
			p_eglGetError());
		goto fail;
	}
	g_ctx = p_eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (g_ctx == EGL_NO_CONTEXT || !g_ctx) {
		fprintf(stderr, "gpu_disp: CreateContext err=%d\n",
			p_eglGetError());
		goto fail;
	}
	if (!p_eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
		fprintf(stderr, "gpu_disp: MakeCurrent err=%d\n",
			p_eglGetError());
		goto fail;
	}
	if (setup_gl() != 0)
		goto fail;
	/* Pace on DRM PageFlip only — SwapInterval(1)+flip double-waits (~30fps). */
	if (p_eglSwapInterval)
		p_eglSwapInterval(g_dpy, 0);

	px = (size_t)g_w * (size_t)g_h;
	g_buf1 = calloc(px, sizeof(lv_color_t));
	g_buf2 = calloc(px, sizeof(lv_color_t));
	if (!g_buf1 || !g_buf2) {
		fprintf(stderr, "gpu_disp: OOM draw buf (%zu px)\n", px);
		goto fail;
	}

	lv_disp_draw_buf_init(&g_draw_buf, g_buf1, g_buf2, (uint32_t)px);
	lv_disp_drv_init(&g_disp_drv);
	g_disp_drv.draw_buf = &g_draw_buf;
	g_disp_drv.flush_cb = gpu_flush;
	g_disp_drv.hor_res = (lv_coord_t)g_w;
	g_disp_drv.ver_res = (lv_coord_t)g_h;
	g_disp_drv.full_refresh = 0; /* dirty rects; present on last flush */
	lv_disp_drv_register(&g_disp_drv);
	gpu_disp_set_refr_60hz();

	/* Prime one black frame onto CRTC */
	p_glClear(GL_COLOR_BUFFER_BIT);
	p_eglSwapBuffers(g_dpy, g_surf);
	present_gbm();

	g_active = 1;
	fprintf(stderr,
		"gpu_disp: OK %dx%d GLES→GBM→DRM PageFlip target~60fps\n",
		g_w, g_h);
	return 0;

fail:
	gpu_disp_deinit();
	return -1;
}
