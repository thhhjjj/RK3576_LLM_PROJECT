/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE6 smoke: open render node → GBM → EGL display/context.
 * Uses dlopen so llm_panel still builds when staging has no EGL headers.
 */

#include "gpu_probe.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Minimal constants (Khronos) — avoid shipping full EGL headers. */
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_DISPLAY ((void *)0)
#define EGL_NO_CONTEXT ((void *)0)
#define EGL_NO_SURFACE ((void *)0)
#define EGL_SUCCESS 0x3000
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_NONE 0x3038
#define EGL_PLATFORM_GBM_KHR 0x31D7

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef unsigned int EGLenum;
typedef int EGLint;
typedef unsigned int EGLBoolean;

struct gbm_device;

int gpu_probe_egl(void)
{
	void *libegl = NULL, *libgbm = NULL;
	int fd = -1;
	struct gbm_device *gbm = NULL;
	EGLDisplay dpy = EGL_NO_DISPLAY;
	EGLContext ctx = EGL_NO_CONTEXT;
	int rc = -1;

	struct gbm_device *(*gbm_create_device)(int) = NULL;
	void (*gbm_device_destroy)(struct gbm_device *) = NULL;

	EGLDisplay (*eglGetPlatformDisplay)(EGLenum, void *, const EGLint *) = NULL;
	EGLDisplay (*eglGetDisplay)(void *) = NULL;
	EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = NULL;
	EGLBoolean (*eglBindAPI)(EGLenum) = NULL;
	EGLContext (*eglCreateContext)(EGLDisplay, void *, EGLContext,
				       const EGLint *) = NULL;
	EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
				     EGLContext) = NULL;
	EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext) = NULL;
	EGLBoolean (*eglTerminate)(EGLDisplay) = NULL;
	EGLint (*eglGetError)(void) = NULL;

	const char *cards[] = {
		"/dev/dri/renderD128",
		"/dev/dri/renderD129",
		"/dev/dri/card0",
		"/dev/dri/card1",
		NULL
	};
	int i;

	/* wayland-gbm Mali links libwayland-client; preload so dlerror is clear */
	if (!dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL) &&
	    !dlopen("libwayland-client.so", RTLD_NOW | RTLD_GLOBAL))
		fprintf(stderr,
			"gpu_probe: warn: no libwayland-client (%s)\n",
			dlerror());

	libgbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!libgbm)
		libgbm = dlopen("libgbm.so", RTLD_NOW | RTLD_GLOBAL);
	libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!libegl)
		libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	/* Pull in mali implementation behind the stubs */
	dlopen("libmali.so.1", RTLD_NOW | RTLD_GLOBAL);

	if (!libgbm || !libegl) {
		fprintf(stderr, "gpu_probe: dlopen gbm/EGL failed (%s)\n",
			dlerror());
		goto out;
	}

	gbm_create_device = dlsym(libgbm, "gbm_create_device");
	gbm_device_destroy = dlsym(libgbm, "gbm_device_destroy");
	eglGetPlatformDisplay = dlsym(libegl, "eglGetPlatformDisplay");
	if (!eglGetPlatformDisplay)
		eglGetPlatformDisplay = dlsym(libegl, "eglGetPlatformDisplayEXT");
	eglGetDisplay = dlsym(libegl, "eglGetDisplay");
	eglInitialize = dlsym(libegl, "eglInitialize");
	eglBindAPI = dlsym(libegl, "eglBindAPI");
	eglCreateContext = dlsym(libegl, "eglCreateContext");
	eglMakeCurrent = dlsym(libegl, "eglMakeCurrent");
	eglDestroyContext = dlsym(libegl, "eglDestroyContext");
	eglTerminate = dlsym(libegl, "eglTerminate");
	eglGetError = dlsym(libegl, "eglGetError");

	if (!gbm_create_device || !eglInitialize || !eglCreateContext) {
		fprintf(stderr, "gpu_probe: missing symbols\n");
		goto out;
	}

	for (i = 0; cards[i]; i++) {
		fd = open(cards[i], O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			fprintf(stderr, "gpu_probe: opened %s\n", cards[i]);
			break;
		}
	}
	if (fd < 0) {
		fprintf(stderr, "gpu_probe: no DRM node\n");
		goto out;
	}

	gbm = gbm_create_device(fd);
	if (!gbm) {
		fprintf(stderr, "gpu_probe: gbm_create_device failed\n");
		goto out;
	}

	if (eglGetPlatformDisplay)
		dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if ((dpy == EGL_NO_DISPLAY || !dpy) && eglGetDisplay)
		dpy = eglGetDisplay((void *)(intptr_t)fd);
	if (dpy == EGL_NO_DISPLAY || !dpy) {
		fprintf(stderr, "gpu_probe: eglGetDisplay failed (err=%d)\n",
			eglGetError ? eglGetError() : -1);
		goto out;
	}

	{
		EGLint major = 0, minor = 0;

		if (!eglInitialize(dpy, &major, &minor)) {
			fprintf(stderr, "gpu_probe: eglInitialize failed (err=%d)\n",
				eglGetError ? eglGetError() : -1);
			goto out;
		}
		fprintf(stderr, "gpu_probe: EGL %d.%d\n", major, minor);
	}

	if (eglBindAPI)
		eglBindAPI(EGL_OPENGL_ES_API);

	{
		const EGLint attrs[] = {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
		};

		/* config=NULL: some mali builds allow context w/o surface for probe */
		ctx = eglCreateContext(dpy, NULL, EGL_NO_CONTEXT, attrs);
		if (ctx == EGL_NO_CONTEXT || !ctx) {
			fprintf(stderr,
				"gpu_probe: eglCreateContext failed (err=%d) — "
				"need full config later for LVGL\n",
				eglGetError ? eglGetError() : -1);
			/* Initialize alone still proves libs + GBM wire-up */
			rc = 0;
			fprintf(stderr,
				"gpu_probe: PARTIAL OK (EGL init, no ES context)\n");
			goto out;
		}
	}

	if (eglMakeCurrent &&
	    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		fprintf(stderr, "gpu_probe: OK (GLES2 context current, no surf)\n");
	else
		fprintf(stderr, "gpu_probe: OK (GLES2 context created)\n");
	rc = 0;

out:
	if (ctx && eglDestroyContext && dpy)
		eglDestroyContext(dpy, ctx);
	if (dpy && eglTerminate)
		eglTerminate(dpy);
	if (gbm && gbm_device_destroy)
		gbm_device_destroy(gbm);
	if (fd >= 0)
		close(fd);
	if (libegl)
		dlclose(libegl);
	if (libgbm)
		dlclose(libgbm);
	return rc;
}
