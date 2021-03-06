/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2013 Collabora, Ltd.
 * Copyright © 2017 Canonical Ltd.
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

#include <unistd.h>
#include <gmock/gmock.h>

#include "helpers.h"

#include "in_process_server.h"

/* tests, that attempt to crash the compositor on purpose */

static struct wl_buffer *
create_bad_shm_buffer(wlcs::Client& client, int width, int height)
{
	struct wl_shm *shm = client.shm();
	int stride = width * 4;
	int size = stride * height;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	int fd;

	fd = wlcs::helpers::create_anonymous_file(size);

	pool = wl_shm_create_pool(shm, fd, size);
	buffer = wl_shm_pool_create_buffer(
        pool,
        0,
        width,
        height,
        stride,
        WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);

	/* Truncate the file to a small size, so that the compositor
	 * will access it out-of-bounds, and hit SIGBUS.
	 */
	assert(ftruncate(fd, 12) == 0);
	close(fd);

	return buffer;
}

using BadBufferTest = wlcs::InProcessServer;

TEST_F(BadBufferTest, test_truncated_shm_file)
{
	using namespace testing;

	wlcs::Client client{the_server()};

	bool buffer_consumed{false};

	auto surface = client.create_visible_surface(200, 200);

	wl_buffer* bad_buffer = create_bad_shm_buffer(client, 200, 200);

	wl_surface_attach(surface, bad_buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, 200, 200);

	surface.add_frame_callback([&buffer_consumed](int) { buffer_consumed = true; });

	wl_surface_commit(surface);

	try
	{
		client.dispatch_until([&buffer_consumed]() { return buffer_consumed; });
	}
	catch (wlcs::ProtocolError const& err)
	{
		wl_buffer_destroy(bad_buffer);
		EXPECT_THAT(err.error_code(), Eq(WL_SHM_ERROR_INVALID_FD));
		EXPECT_THAT(err.interface(), Eq(&wl_buffer_interface));
		return;
	}

	FAIL() << "Expected protocol error not raised";
}
