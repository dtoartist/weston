/*
 * Copyright @ 2019 YoungJun Jo
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014,2018 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <sys/ioctl.h> /* for ion ioctls */
#include <sys/mman.h> /* for mmap */

#include <wayland-client.h>
#include "shared/platform.h"
#include "shared/zalloc.h"
#include "xdg-shell-unstable-v6-client-protocol.h"
#include "fullscreen-shell-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "weston-egl-ext.h"

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

/* tcc specific macro */
#define SIZE_ALIGN(value, base) (((value) + ((base)-1)) & ~((base)-1))
#define GPU_RGB_ALIGN_SZ (64u)
#define GPU_YUV_ALIGN_SZ (16u)

#define ION_DEVICE "/dev/ion"

/*
 * ION type structure and defines. It is refered from
 * kernel/drivers/staging/android/uapi/ion.h
 */

struct ion_allocation_data
{
	__u64 len;
	__u32 heap_id_mask;
	__u32 flags;
	__u32 fd;
	__u32 unused;
};

#define MAX_HEAP_NAME                   32

struct ion_heap_data {
        char name[MAX_HEAP_NAME];
        __u32 type;
        __u32 heap_id;
        __u32 reserved0;
        __u32 reserved1;
        __u32 reserved2;
};

struct ion_heap_query {
        __u32 cnt; /* Total number of heaps to be copied */
        __u32 reserved0; /* align to 64bits */
        __u64 heaps; /* buffer to be populated */
        __u32 reserved1;
        __u32 reserved2;
};

/* TCC */
struct ion_tcc_phys_data {
	__u32 heap_mask;
        int dmabuf_fd;
        unsigned long paddr;
        size_t len;
};

/* ION ioctls for kernel v4.14 */
#define ION_IOC_MAGIC 'I'

#define ION_IOC_ALLOC		_IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_HEAP_QUERY	_IOWR(ION_IOC_MAGIC, 8, struct ion_heap_query)
#define ION_IOC_PHYS		_IOWR(ION_IOC_MAGIC, 100, struct ion_tcc_phys_data)

/* ION heap types */
enum ion_heap_type
{
	ION_HEAP_TYPE_SYSTEM,
	ION_HEAP_TYPE_SYSTEM_CONTIG,
	ION_HEAP_TYPE_CARVEOUT,
	ION_HEAP_TYPE_CARVEOUT_CAM,
	ION_HEAP_TYPE_CHUNK,
	ION_HEAP_TYPE_DMA,
	ION_HEAP_TYPE_CUSTOM,
};

#define MAX_HEAP_COUNT		ION_HEAP_TYPE_CUSTOM
#define ION_NUM_HEAP_IDS	(sizeof(unsigned int) * 8)

#define HEAP_MASK_FROM_TYPE(type) (1 << type)

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct zxdg_shell_v6 *shell;
	struct zwp_fullscreen_shell_v1 *fshell;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	int argb8888_format_found;
	int req_dmabuf_immediate;
	struct {
		EGLDisplay display;
		EGLContext context;
		EGLConfig config;
		PFNEGLCREATEIMAGEKHRPROC create_image;
		PFNEGLDESTROYIMAGEKHRPROC destroy_image;
		PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d;
	} egl;
	struct {
		int dev_fd;
	} ion_device;
};

struct buffer {
	struct display *display;
	struct wl_buffer *buffer;
	int busy;

	struct ion_allocation_data ion_data;
	unsigned char *vaddr;

	int width;
	int height;
	uint32_t stride;
	int format;

	EGLImageKHR egl_image;
	GLuint gl_texture;
	GLuint gl_fbo;
};

#define NUM_BUFFERS 3

struct window {
	struct display *display;
	int width, height;
	struct wl_surface *surface;
	struct zxdg_surface_v6 *xdg_surface;
	struct zxdg_toplevel_v6 *xdg_toplevel;
	struct buffer buffers[NUM_BUFFERS];
	struct wl_callback *callback;
	bool initialized;
	bool wait_for_configure;
};

static sig_atomic_t running = 1;

static uint32_t ion_user_type = ION_HEAP_TYPE_SYSTEM;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time);

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;

	mybuf->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static void
buffer_free(struct buffer *buf)
{
	if (buf->gl_fbo)
		glDeleteFramebuffers(1, &buf->gl_fbo);

	if (buf->gl_texture)
		glDeleteTextures(1, &buf->gl_texture);

	if (buf->egl_image) {
		buf->display->egl.destroy_image(buf->display->egl.display,
						buf->egl_image);
	}

	if (buf->buffer)
		wl_buffer_destroy(buf->buffer);

	if (buf->vaddr)
		munmap(buf->vaddr, buf->ion_data.len);

	if (buf->ion_data.fd >= 0)
		close(buf->ion_data.fd);
}

static void
create_succeeded(void *data,
		 struct zwp_linux_buffer_params_v1 *params,
		 struct wl_buffer *new_buffer)
{
	struct buffer *buffer = data;

	buffer->buffer = new_buffer;
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

	zwp_linux_buffer_params_v1_destroy(params);
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
	struct buffer *buffer = data;

	buffer->buffer = NULL;
	running = 0;

	zwp_linux_buffer_params_v1_destroy(params);

	fprintf(stderr, "Error: zwp_linux_buffer_params.create failed.\n");
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
	create_succeeded,
	create_failed
};

static bool
create_fbo_for_buffer(struct display *display, struct buffer *buffer)
{
	EGLint attribs[] = {
		EGL_WIDTH, buffer->width,
		EGL_HEIGHT, buffer->height,
		EGL_LINUX_DRM_FOURCC_EXT, buffer->format,
		EGL_DMA_BUF_PLANE0_FD_EXT, buffer->ion_data.fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (int) buffer->stride,
		EGL_NONE
	};

	buffer->egl_image = display->egl.create_image(display->egl.display,
						      EGL_NO_CONTEXT,
						      EGL_LINUX_DMA_BUF_EXT,
						      NULL, attribs);
	if (buffer->egl_image == EGL_NO_IMAGE) {
		fprintf(stderr, "EGLImageKHR creation failed\n");
		return false;
	}

	eglMakeCurrent(display->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			display->egl.context);

	glGenTextures(1, &buffer->gl_texture);
	glBindTexture(GL_TEXTURE_2D, buffer->gl_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	display->egl.image_target_texture_2d(GL_TEXTURE_2D, buffer->egl_image);

	glGenFramebuffers(1, &buffer->gl_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, buffer->gl_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, buffer->gl_texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "FBO creation failed\n");
		return false;
	}

	return true;
}


static int
create_dmabuf_buffer(struct display *display, struct buffer *buffer,
		     int width, int height, int format)
{
	static const uint32_t flags = 0;
	static const uint64_t modifier = 0;
	struct zwp_linux_buffer_params_v1 *params;

	struct ion_heap_query query;
	struct ion_heap_data heap_data[MAX_HEAP_COUNT];
	unsigned int heap_id;
	int i;

	buffer->display = display;
	buffer->width = width;
	buffer->height = height;
	buffer->format = format;
	buffer->vaddr = 0;

	memset(&query, 0, sizeof(query));
	query.cnt = MAX_HEAP_COUNT;
	query.heaps = (unsigned long int)&heap_data[0];
	/* Query ION heap_id_mask from ION heap */
	if (ioctl(display->ion_device.dev_fd, ION_IOC_HEAP_QUERY, &query) < 0)
	{
		fprintf(stderr, "error: ION_IOC_HEAP_QUERY failed\n");
		goto error;
	}

	heap_id = MAX_HEAP_COUNT +1;
	for (i=0; i<query.cnt; i++) {
		if (heap_data[i].type == ion_user_type) {
			printf("--------------------------------------\n");
			printf("heap type: %d\n", heap_data[i].type);
			printf("  heap id: %d\n", heap_data[i].heap_id);
			printf("heap name: %s\n", heap_data[i].name);
			printf("--------------------------------------\n");
			heap_id = heap_data[i].heap_id;
			break;
		}
	}

	if (heap_id > MAX_HEAP_COUNT) {
		fprintf(stderr, "error: heap type does not exists\n");
		goto error;
	}

	buffer->ion_data.len = width * height * 4; /* hardcoded bpp ARGB8888 = 32/8 = 4 */
	buffer->ion_data.heap_id_mask = HEAP_MASK_FROM_TYPE(heap_id);
	buffer->ion_data.flags = 0;

	if (ioctl(display->ion_device.dev_fd, ION_IOC_ALLOC, &buffer->ion_data) < 0)
	{
		fprintf(stderr, "error: ION_IOC_ALLOC failed\n");
		goto error;
	}

	buffer->stride = SIZE_ALIGN(buffer->width * 4, GPU_RGB_ALIGN_SZ);

	if (buffer->ion_data.fd == 0) {
		fprintf(stderr, "error: get dmabuf_fd failed\n");
		goto error;
	}

	/* If virtual address is needed.. */
	{
		buffer->vaddr = (unsigned char *)mmap(
			(void *)0, buffer->ion_data.len, PROT_READ | PROT_WRITE,
			MAP_SHARED, buffer->ion_data.fd, 0);

		if (buffer->vaddr == MAP_FAILED) {
			fprintf(stderr, "error: mmap failed\n");
			goto error;
		}

		printf("buffer address 0x%08x\n", (unsigned int)buffer->vaddr);
	}

	params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
	zwp_linux_buffer_params_v1_add(params,
				       buffer->ion_data.fd,
				       0, /* plane_idx */
				       0, /* offset */
				       buffer->stride,
				       modifier >> 32,
				       modifier & 0xffffffff);
	zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buffer);
	zwp_linux_buffer_params_v1_create(params,
					  buffer->width,
					  buffer->height,
					  format,
					  flags);

	if (!create_fbo_for_buffer(display, buffer))
		goto error;

	return 0;
error:
	buffer_free(buffer);
	return -1;
}

static void
xdg_surface_handle_configure(void *data, struct zxdg_surface_v6 *surface,
			     uint32_t serial)
{
	struct window *window = data;

	zxdg_surface_v6_ack_configure(surface, serial);

	if (window->initialized && window->wait_for_configure)
		redraw(window, NULL, 0);
	window->wait_for_configure = false;
}

static const struct zxdg_surface_v6_listener xdg_surface_listener = {
	xdg_surface_handle_configure,
};

static void
xdg_toplevel_handle_configure(void *data, struct zxdg_toplevel_v6 *toplevel,
			      int32_t width, int32_t height,
			      struct wl_array *states)
{
}

static void
xdg_toplevel_handle_close(void *data, struct zxdg_toplevel_v6 *xdg_toplevel)
{
	running = 0;
}

static const struct zxdg_toplevel_v6_listener xdg_toplevel_listener = {
	xdg_toplevel_handle_configure,
	xdg_toplevel_handle_close,
};

static struct window *
create_window(struct display *display, int width, int height)
{
	struct window *window;
	int i;
	int ret;

	window = zalloc(sizeof *window);
	if (!window)
		return NULL;

	window->callback = NULL;
	window->display = display;
	window->width = width;
	window->height = height;
	window->surface = wl_compositor_create_surface(display->compositor);

	if (display->shell) {
		window->xdg_surface =
			zxdg_shell_v6_get_xdg_surface(display->shell,
						      window->surface);

		assert(window->xdg_surface);

		zxdg_surface_v6_add_listener(window->xdg_surface,
					     &xdg_surface_listener, window);

		window->xdg_toplevel =
			zxdg_surface_v6_get_toplevel(window->xdg_surface);

		assert(window->xdg_toplevel);

		zxdg_toplevel_v6_add_listener(window->xdg_toplevel,
					      &xdg_toplevel_listener, window);

		zxdg_toplevel_v6_set_title(window->xdg_toplevel, "simple-dmabuf-ion");

		window->wait_for_configure = true;
		wl_surface_commit(window->surface);
	} else if (display->fshell) {
		zwp_fullscreen_shell_v1_present_surface(display->fshell,
							window->surface,
							ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
							NULL);
	} else {
		assert(0);
	}

	for (i = 0; i < NUM_BUFFERS; ++i) {
		ret = create_dmabuf_buffer(display, &window->buffers[i],
		                           width, height, DRM_FORMAT_ARGB8888);

		if (ret < 0)
			return NULL;
	}

	return window;
}

static void
destroy_window(struct window *window)
{
	int i;

	if (window->callback)
		wl_callback_destroy(window->callback);

	for (i = 0; i < NUM_BUFFERS; i++) {
		if (window->buffers[i].buffer)
			buffer_free(&window->buffers[i]);
	}

	if (window->xdg_toplevel)
		zxdg_toplevel_v6_destroy(window->xdg_toplevel);
	if (window->xdg_surface)
		zxdg_surface_v6_destroy(window->xdg_surface);
	wl_surface_destroy(window->surface);
	free(window);
}

static struct buffer *
window_next_buffer(struct window *window)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++)
		if (!window->buffers[i].busy)
			return &window->buffers[i];

	return NULL;
}

static const struct wl_callback_listener frame_listener;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	/* With a 60Hz redraw rate this completes a cycle in 3 seconds */
	static const int MAX_STEP = 180;
	static int step = 0;
	static int step_dir = 1;
	struct window *window = data;
	struct buffer *buffer;

	buffer = window_next_buffer(window);
	if (!buffer) {
		fprintf(stderr,
			!callback ? "Failed to create the first buffer.\n" :
			"All buffers busy at redraw(). Server bug?\n");
		abort();
	}

	/* Direct all GL draws to the buffer through the FBO */
	glBindFramebuffer(GL_FRAMEBUFFER, buffer->gl_fbo);

	/* Cycle between 0 and MAX_STEP */
	step += step_dir;
	if (step == 0 || step == MAX_STEP)
		step_dir = -step_dir;

	glClearColor(0.0,
		     (float) step / MAX_STEP,
		     1.0 - (float) step / MAX_STEP,
		     1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();

	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(window->surface, 0, 0, window->width, window->height);

	if (callback)
		wl_callback_destroy(callback);

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, window);
	wl_surface_commit(window->surface);
	buffer->busy = 1;
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format)
{
	struct display *d = data;

	switch (format) {
	case DRM_FORMAT_ARGB8888:
		d->argb8888_format_found = 1;
		break;
	default:
		break;
	}
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	dmabuf_format,
};

static void
xdg_shell_ping(void *data, struct zxdg_shell_v6 *shell, uint32_t serial)
{
	zxdg_shell_v6_pong(shell, serial);
}

static const struct zxdg_shell_v6_listener xdg_shell_listener = {
	xdg_shell_ping,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry,
					 id, &wl_compositor_interface, 1);
	} else if (strcmp(interface, "zxdg_shell_v6") == 0) {
		d->shell = wl_registry_bind(registry,
					    id, &zxdg_shell_v6_interface, 1);
		zxdg_shell_v6_add_listener(d->shell, &xdg_shell_listener, d);
	} else if (strcmp(interface, "zwp_fullscreen_shell_v1") == 0) {
		d->fshell = wl_registry_bind(registry,
					     id, &zwp_fullscreen_shell_v1_interface, 1);
	} else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
		d->dmabuf = wl_registry_bind(registry,
					     id, &zwp_linux_dmabuf_v1_interface, 1);
		zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener, d);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static void
destroy_display(struct display *display)
{
	if (display->ion_device.dev_fd)
		close(display->ion_device.dev_fd);

	if (display->egl.context != EGL_NO_CONTEXT)
		eglDestroyContext(display->egl.display, display->egl.context);

	if (display->egl.display != EGL_NO_DISPLAY)
		eglTerminate(display->egl.display);

	if (display->dmabuf)
		zwp_linux_dmabuf_v1_destroy(display->dmabuf);

	if (display->shell)
		zxdg_shell_v6_destroy(display->shell);

	if (display->fshell)
		zwp_fullscreen_shell_v1_release(display->fshell);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	if (display->registry)
		wl_registry_destroy(display->registry);

	if (display->display) {
		wl_display_flush(display->display);
		wl_display_disconnect(display->display);
	}

	free(display);
}

static bool
display_set_up_egl(struct display *display)
{
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLint major, minor;
	const char *egl_extensions = NULL;
	const char *gl_extensions = NULL;

	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	EGLint n, count, i, size;
	EGLConfig *configs;
	EGLBoolean ret;
	EGLSurface egl_surface;
	struct wl_surface *surface;
	struct wl_egl_window *native;

	display->egl.display =
		weston_platform_get_egl_display(EGL_PLATFORM_WAYLAND_KHR,
						display->display, NULL);
	if (display->egl.display == EGL_NO_DISPLAY) {
		fprintf(stderr, "Failed to create EGLDisplay\n");
		goto error;
	}

	if (eglInitialize(display->egl.display, &major, &minor) == EGL_FALSE) {
		fprintf(stderr, "Failed to initialize EGLDisplay\n");
		goto error;
	}

	if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
		fprintf(stderr, "Failed to bind OpenGL ES API\n");
		goto error;
	}

	egl_extensions = eglQueryString(display->egl.display, EGL_EXTENSIONS);
	assert(egl_extensions != NULL);

	if (!weston_check_egl_extension(egl_extensions,
					"EGL_EXT_image_dma_buf_import")) {
		fprintf(stderr, "EGL_EXT_image_dma_buf_import not supported\n");
		goto error;
	}

	if (!eglGetConfigs(display->egl.display, NULL, 0, &count) || count < 1)
		assert(0);

	configs = calloc(count, sizeof *configs);
	assert(configs);

	ret = eglChooseConfig(display->egl.display, config_attribs,
			      configs, count, &n);
	assert(ret && n >= 1);

	for (i = 0; i < n; i++) {
		eglGetConfigAttrib(display->egl.display,
				   configs[i], EGL_BUFFER_SIZE, &size);
		if (size == 32) {
			display->egl.config = configs[i];
			break;
		}
	}
	free(configs);
	if (display->egl.config == NULL) {
		fprintf(stderr, "did not find config with buffer size 32\n");
		exit(EXIT_FAILURE);
	}
#if 1 // temporary code
	surface = wl_compositor_create_surface(display->compositor);

	native = wl_egl_window_create(surface, 256, 256);
	egl_surface = weston_platform_create_egl_surface(display->egl.display,
							 display->egl.config,
							 native, NULL);
#endif

	display->egl.context = eglCreateContext(display->egl.display,
						display->egl.config,
						EGL_NO_CONTEXT, context_attribs);
	assert(display->egl.context);

	ret = eglMakeCurrent(display->egl.display, egl_surface, egl_surface,
			     display->egl.context);
	if (ret == EGL_FALSE) {
		fprintf(stderr, "Failed to make EGL context current\n");
		goto error;
	}

	gl_extensions = (const char *) glGetString(GL_EXTENSIONS);
	assert(gl_extensions != NULL);

	if (!weston_check_egl_extension(gl_extensions,
					"GL_OES_EGL_image")) {
		fprintf(stderr, "GL_OES_EGL_image not suported\n");
		goto error;
	}

	display->egl.create_image =
		(void *) eglGetProcAddress("eglCreateImageKHR");
	assert(display->egl.create_image);

	display->egl.destroy_image =
		(void *) eglGetProcAddress("eglDestroyImageKHR");
	assert(display->egl.destroy_image);

	display->egl.image_target_texture_2d =
		(void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	assert(display->egl.image_target_texture_2d);
#if 1 // temporary code
	eglDestroySurface(display->egl.display, egl_surface);
	wl_egl_window_destroy(native);
	wl_surface_destroy(surface);
#endif
	return true;

error:
	return false;
}

static bool
display_set_up_ion(struct display *display, char const* ion_device_node)
{
	display->ion_device.dev_fd = open(ion_device_node, O_RDWR);
	if (display->ion_device.dev_fd < 0) {
		fprintf(stderr, "Failed to open ion device node %s\n",
			ion_device_node);
		return false;
	}

	return true;
}

static struct display *
create_display(void)
{
	struct display *display = NULL;

	display = zalloc(sizeof *display);
	if (display == NULL) {
		fprintf(stderr, "out of memory\n");
		goto error;
	}

	display->display = wl_display_connect(NULL);
	assert(display->display);

	/* XXX: fake, because the compositor does not yet advertise anything */
	display->argb8888_format_found = 1;

	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry,
				 &registry_listener, display);
	wl_display_roundtrip(display->display);
	if (display->dmabuf == NULL) {
		fprintf(stderr, "No zwp_linux_dmabuf global\n");
		goto error;
	}

	wl_display_roundtrip(display->display);

	if (!display->argb8888_format_found) {
		fprintf(stderr, "format ARGB8888 is not available\n");
		goto error;
	}

	if (!display_set_up_egl(display))
		goto error;

	if (!display_set_up_ion(display, ION_DEVICE))
		goto error;

	return display;

error:
	if (display != NULL)
		destroy_display(display);
	return NULL;
}

static void
signal_int(int signum)
{
	running = 0;
}

int
main(int argc, char **argv)
{
	struct sigaction sigint;
	struct display *display;
	struct window *window;
	int ret = 0;

	if (argc == 2) {
		if (atoi(argv[1]) == 1) {
			ion_user_type = ION_HEAP_TYPE_CARVEOUT;
			printf("ion type = ION_HEAP_TYPE_CARVEOUT\n");
		}
	} else {
		printf("The default ion mask = ION_HEAP_TYPE_SYSTEM\n");
		printf("If you want to ION_HEAP_TYPE_CARVEOUT => # weston-simple-dmabuf-ion 1\n");
	}

	display = create_display();
	if (!display)
		return 1;
	window = create_window(display, 256, 256);
	if (!window)
		return 1;

	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	/* Here we retrieve the linux-dmabuf objects if executed without immed,
	 * or error */
	wl_display_roundtrip(display->display);

	if (!running)
		return 1;

	window->initialized = true;

	if (!window->wait_for_configure)
		redraw(window, NULL, 0);

	while (running && ret != -1)
		ret = wl_display_dispatch(display->display);

	fprintf(stderr, "simple-dmabuf-ion exiting\n");
	destroy_window(window);
	destroy_display(display);

	return 0;
}
