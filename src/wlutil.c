/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "wlutil.h"

static int extra_fd = -1;
static void (*extra_cb)(void);
static volatile sig_atomic_t interrupted;
static const sigset_t *wait_mask;

void display_set_extra_fd(int fd, void (*cb)(void))
{
	extra_fd = fd;
	extra_cb = cb;
}

void display_interrupt(void)
{
	interrupted = 1;
}

int display_interrupted(void)
{
	return interrupted;
}

void display_set_wait_mask(const sigset_t *mask)
{
	wait_mask = mask;
}

double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int display_wait(struct wl_display *dpy, int (*done)(void *), void *arg, int timeout_ms)
{
	double end = now_ms() + timeout_ms;

	while (!done(arg)) {
		struct pollfd p[2] = { { wl_display_get_fd(dpy), POLLIN, 0 }, { extra_fd, POLLIN, 0 } };
		int left = timeout_ms < 0 ? -1 : (int)(end - now_ms()), pr;

		if (interrupted)
			return -1;
		while (wl_display_prepare_read(dpy) != 0)
			wl_display_dispatch_pending(dpy);
		if (wl_display_flush(dpy) < 0) {
			if (errno != EAGAIN) {
				wl_display_cancel_read(dpy);
				return -1;
			}
			p[0].events |= POLLOUT;
		}
		if (timeout_ms >= 0 && left <= 0) {
			pr = 0;
		} else {
			struct timespec ts = { left / 1000, (left % 1000) * 1000000L };

			pr = ppoll(p, 2, left < 0 ? NULL : &ts, wait_mask);
		}
		if (pr < 0 && errno == EINTR) {
			wl_display_cancel_read(dpy);
			continue;
		}
		if (pr <= 0) {
			wl_display_cancel_read(dpy);
			return -1;
		}
		if (p[0].revents & POLLIN) {
			if (wl_display_read_events(dpy) < 0)
				return -1;
		} else {
			wl_display_cancel_read(dpy);
		}
		if (wl_display_dispatch_pending(dpy) < 0)
			return -1;
		if (extra_fd >= 0 && (p[1].revents & POLLIN) && extra_cb)
			extra_cb();
	}
	return 0;
}

struct wl_buffer *shm_buffer_create(struct wl_shm *shm, int w, int h, uint32_t format,
		void **data, size_t *size)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buf;
	int fd = memfd_create("skippy-wl", MFD_CLOEXEC);
	void *map;

	if (fd < 0)
		return NULL;
	*size = (size_t)w * 4 * (size_t)h;
	if (ftruncate(fd, (off_t)*size) < 0) {
		close(fd);
		return NULL;
	}
	map = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	pool = wl_shm_create_pool(shm, fd, (int32_t)*size);
	buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	*data = map;
	return buf;
}
