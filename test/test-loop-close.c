/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "task.h"

static uv_timer_t timer_handle;

static void timer_cb(uv_timer_t* handle) {
  ASSERT(handle);
  uv_stop(handle->loop);
}


TEST_IMPL(loop_close) {
  int r;
  uv_loop_t loop;

  loop.data = &loop;
  ASSERT(0 == uv_loop_init(&loop));
  ASSERT(loop.data == (void*) &loop);

  uv_timer_init(&loop, &timer_handle);
  uv_timer_start(&timer_handle, timer_cb, 100, 100);

  ASSERT(UV_EBUSY == uv_loop_close(&loop));

  uv_run(&loop, UV_RUN_DEFAULT);

  uv_close((uv_handle_t*) &timer_handle, NULL);
  r = uv_run(&loop, UV_RUN_DEFAULT);
  ASSERT(r == 0);

  ASSERT(loop.data == (void*) &loop);
  ASSERT(0 == uv_loop_close(&loop));
  ASSERT(loop.data == (void*) &loop);

  return 0;
}

static uv_file fd;
static uv_fs_t fs_req;
static uv_fs_t close_req;
static char test_buf[] = "test-buffer\n";
static uv_buf_t iov;

static void write_cb(uv_fs_t* req) {
  int r;
  ASSERT(req->result >= 0); /* FIXME(bnoordhuis) Check if requested size? */
  uv_fs_req_cleanup(req);

  r = uv_fs_close(uv_default_loop(), &close_req, fd, NULL);
  ASSERT(r == 0);
  uv_fs_req_cleanup(&close_req);
}

TEST_IMPL(loop_instant_close) {
  int r;
  static uv_loop_t loop;
  static uv_work_t req;
  ASSERT(0 == uv_loop_init(&loop));

  r = uv_fs_open(uv_default_loop(),
                 &fs_req,
                 "test_file",
                 O_WRONLY | O_CREAT,
                 S_IRUSR | S_IWUSR,
                 NULL);
  ASSERT(r >= 0);
  uv_fs_req_cleanup(&fs_req);
  iov = uv_buf_init(test_buf, sizeof(test_buf));
  r = uv_fs_write(uv_default_loop(), &fs_req, r, &iov, 1, -1, write_cb);
  ASSERT(r == 0);

  MAKE_VALGRIND_HAPPY();
  return 0;
}
