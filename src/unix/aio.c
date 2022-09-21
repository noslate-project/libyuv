
#include "internal.h"
#include "uv.h"

#if defined(__linux__)

#include <assert.h>
#include <errno.h>
#include <linux/aio_abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/**
 * Total number of aio nr is shared across the system.
 */
#ifndef UV_AIO_NR_EVENTS
#define UV_AIO_NR_EVENTS 128
#endif

static inline int uv__io_setup(unsigned n, uv__aio_context_t* c) {
  return syscall(__NR_io_setup, n, c);
}

static inline int uv__io_destroy(uv__aio_context_t c) {
  return syscall(__NR_io_destroy, c);
}

static inline int uv__io_submit(uv__aio_context_t c, long n, struct iocb** b) {
  return syscall(__NR_io_submit, c, n, b);
}

static inline int uv__io_getevents(uv__aio_context_t c,
                                   long min,
                                   long max,
                                   struct io_event* e,
                                   struct timespec* t) {
  return syscall(__NR_io_getevents, c, min, max, e, t);
}

static inline int uv__io_cancel(uv__aio_context_t c,
                                struct iocb* b,
                                struct io_event* result) {
  return syscall(__NR_io_cancel, c, b, result);
}

static int uv__aio_start(uv__aio_t* w);

int uv__aio_init(uv_loop_t* loop, uv__aio_t* w, uv__aio_cb aio_cb) {
  w->loop = loop;
  w->aio_io_watcher.fd = -1;
  w->aio_wfd = -1;
  w->aio_ctx = 0;
  w->aio_cb = aio_cb;
  QUEUE_INIT(&w->iocb_pending_queue);

  int err;
  err = uv__aio_start(w);
  if (err) return err;

  return 0;
}

void uv__aio_drain_pending_queue(uv__aio_t* w) {
  QUEUE* q;
  uv_fs_t* req;
  unsigned int i, pending_count;

  while (!QUEUE_EMPTY(&w->iocb_pending_queue)) {
    q = QUEUE_HEAD(&w->iocb_pending_queue);
    req = QUEUE_DATA(q, uv_fs_t, iocb_pending_queue);

    pending_count =
        MIN(req->iocbs_count - req->submitted_iocbs_count, UV_AIO_NR_EVENTS);
    struct iocb** iocbs = uv__calloc(pending_count, sizeof(struct iocb*));
    for (i = 0; i < pending_count &&
                i + req->submitted_iocbs_count < req->iocbs_count;
         i++) {
      iocbs[i] = req->iocbs + i + req->submitted_iocbs_count;
    }

    int r = uv__io_submit(w->aio_ctx, pending_count, iocbs);
    uv__free(iocbs);
    if (r < 0 && errno == EAGAIN) {
      break;
    } else if (r < 0) {
      assert(errno);
      assert(r >= 0);
      break;
    }

    req->submitted_iocbs_count += r;
    if (req->submitted_iocbs_count >= req->iocbs_count) {
      QUEUE_REMOVE(q);
    }
  }
}

void uv__aio_submit(uv_loop_t* loop,
                    uv_fs_t* req,
                    void (*done)(struct uv__work* w, int status)) {
  req->work_req.loop = loop;
  req->work_req.done = done;

  unsigned int i;

  if (req->iocbs == NULL) {
    req->iocbs = uv__calloc(req->nbufs, sizeof(struct iocb));
    assert(req->iocbs);

    struct iocb* ctrl_blk = req->iocbs;
    off_t offset = req->off;
    if (offset < 0) offset = 0;
    for (i = 0; i < req->nbufs; i++, ctrl_blk++) {
      switch (req->fs_type) {
        case UV_FS_READ:
          ctrl_blk->aio_lio_opcode = IOCB_CMD_PREAD;
          break;

        case UV_FS_WRITE:
          ctrl_blk->aio_lio_opcode = IOCB_CMD_PWRITE;
          break;

        default:
          UNREACHABLE();
          break;
      }

      ctrl_blk->aio_fildes = req->file;
      ctrl_blk->aio_buf = (uint64_t)req->bufs[i].base;
      ctrl_blk->aio_offset = offset;
      ctrl_blk->aio_nbytes = req->bufs[i].len;
      ctrl_blk->aio_data = (uint64_t)req;
      ctrl_blk->aio_flags = IOCB_FLAG_RESFD;
      ctrl_blk->aio_resfd = loop->wq_aio.aio_io_watcher.fd;
      offset += req->bufs[i].len;
    }

    req->iocbs_count = req->nbufs;
    req->submitted_iocbs_count = 0;
  }

  QUEUE_INSERT_TAIL(&loop->wq_aio.iocb_pending_queue,
                    &req->iocb_pending_queue);
  uv__aio_drain_pending_queue(&loop->wq_aio);
}

void uv__aio_work_done(uv__aio_t* w) {
  struct timespec tms = {0};
  int i, r;

  long n = UV_AIO_NR_EVENTS;
  struct io_event* events = uv__calloc(n, sizeof(struct io_event));
  do {
    r = uv__io_getevents(w->aio_ctx, 0, n, events, &tms);

    for (i = 0; i < r; i++) {
      uv_fs_t* req = (uv_fs_t*)events[i].data;

      if (events[i].res >= 0 && req->result >= 0) {
        req->result += events[i].res;
      } else {
        req->result = events[i].res;
      }

      req->done_iocbs_count++;
      if (req->done_iocbs_count < req->iocbs_count) {
        continue;
      }
      struct uv__work* w = &req->work_req;
      if (w->done) {
        w->done(w, 0);
      }
    }
  } while (r > 0);

  uv__free(events);

  uv__aio_drain_pending_queue(w);
}

static void uv__aio_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  char buf[1024];
  ssize_t r;
  int64_t n = 0;

  assert(w == &loop->wq_aio.aio_io_watcher);

  for (;;) {
    r = read(w->fd, buf, sizeof(buf));
    if (r > 0) n += r;

    if (r == sizeof(buf))
      continue;

    else if (r != -1)
      break;

    else if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;

    else if (errno == EINTR)
      continue;

    abort();
  }

  if (loop->wq_aio.aio_cb != NULL) {
    loop->wq_aio.aio_cb(&loop->wq_aio);
  }
}

static int uv__aio_start(uv__aio_t* w) {
  int pipefd[2];
  int err;

  err = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (err < 0) {
    perror("eventfd");
    return UV__ERR(errno);
  }

  pipefd[0] = err;
  pipefd[1] = -1;

  err = uv__io_setup(UV_AIO_NR_EVENTS, &w->aio_ctx);
  if (err < 0) {
    perror("io_setup");
    return err;
  }

  uv__io_init(&w->aio_io_watcher, uv__aio_io, pipefd[0]);
  uv__io_start(w->loop, &w->aio_io_watcher, POLLIN);
  w->aio_wfd = pipefd[1];

  return 0;
}

void uv__aio_stop(uv_loop_t* loop, uv__aio_t* w) {
  uv__io_stop(loop, &w->aio_io_watcher, POLLIN);
}

void uv__aio_close(uv__aio_t* w) {
  uv__aio_stop(w->loop, w);
  uv__io_close(w->loop, &w->aio_io_watcher);
}

#else

int uv__aio_init(uv_loop_t* loop, uv__aio_t* w, uv__aio_cb aio_cb) {
  assert(0);
  return -1;
}

void uv__aio_close(uv__aio_t* w) {
  assert(0);
}

#endif
