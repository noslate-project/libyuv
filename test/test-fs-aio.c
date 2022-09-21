/**
 * NOTE: This file is a duplication of test/test-fs.c.
 * When io_uring is adopted, i.e. all fs operations can be applied in a fully
 * async way, we can remove this duplication and use test/test-fs.c instead.
 */

#include "task.h"
#include "uv.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* INT_MAX, PATH_MAX, IOV_MAX */
#include <string.h> /* memset */
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h> /* unlink, rmdir, etc. */
#else
#include <direct.h>
#include <io.h>
#include <winioctl.h>
#ifndef ERROR_SYMLINK_NOT_SUPPORTED
#define ERROR_SYMLINK_NOT_SUPPORTED 1464
#endif
#define unlink _unlink
#define rmdir _rmdir
#define open _open
#define write _write
#define close _close
#ifndef stat
#define stat _stati64
#endif
#ifndef lseek
#define lseek _lseek
#endif
#endif

#define TOO_LONG_NAME_LENGTH 65536
#define PATHMAX 4096

typedef struct {
  const char* path;
  double atime;
  double mtime;
} utime_check_t;

static int read_cb_count;
static int write_cb_count;
static int fs_write_alotof_bufs_async_cb_count;

static uv_loop_t* loop;

static uv_fs_t open_req1;
static uv_fs_t read_req;
static uv_fs_t write_req;
static uv_fs_t close_req;
static uv_fs_t fdatasync_req;
static uv_fs_t ftruncate_req;
static uv_fs_t rename_req;
static uv_fs_t stat_req;
static uv_fs_t unlink_req;

static char buf[32];
static char buf2[32];
static char test_buf[] = "test-buffer\n";
static char test_buf2[] = "second-buffer\n";
static uv_buf_t iov;

#ifdef _WIN32
int uv_test_getiovmax(void) {
  return INT32_MAX; /* Emulated by libuv, so no real limit. */
}
#else
int uv_test_getiovmax(void) {
#if defined(IOV_MAX)
  return IOV_MAX;
#elif defined(_SC_IOV_MAX)
  static int iovmax = -1;
  if (iovmax == -1) {
    iovmax = sysconf(_SC_IOV_MAX);
    /* On some embedded devices (arm-linux-uclibc based ip camera),
     * sysconf(_SC_IOV_MAX) can not get the correct value. The return
     * value is -1 and the errno is EINPROGRESS. Degrade the value to 1.
     */
    if (iovmax == -1) iovmax = 1;
  }
  return iovmax;
#else
  return 1024;
#endif
}
#endif

#ifdef _WIN32
/*
 * This tag and guid have no special meaning, and don't conflict with
 * reserved ids.
 */
static unsigned REPARSE_TAG = 0x9913;
static GUID REPARSE_GUID = {0x1bf6205f,
                            0x46ae,
                            0x4527,
                            {0xb1, 0x0c, 0xc5, 0x09, 0xb7, 0x55, 0x22, 0x80}};
#endif

static void fail_cb(uv_fs_t* req) {
  FATAL("fail_cb should not have been called");
}

static void read_cb(uv_fs_t* req) {
  int r;
  ASSERT(req == &read_req);
  ASSERT(req->fs_type == UV_FS_READ);
  ASSERT(req->result >= 0);  /* FIXME(bnoordhuis) Check if requested size? */
  read_cb_count++;
  uv_fs_req_cleanup(req);

  ASSERT(strcmp(buf, test_buf) == 0);
  r = uv_fs_close(loop, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  uv_fs_req_cleanup(&close_req);
}

static void write_cb(uv_fs_t* req) {
  int r;
  ASSERT(req == &write_req);
  ASSERT(req->fs_type == UV_FS_WRITE);
  ASSERT(req->result >= 0); /* FIXME(bnoordhuis) Check if requested size? */
  write_cb_count++;
  uv_fs_req_cleanup(req);
  r = uv_fs_fdatasync(loop, &fdatasync_req, open_req1.result, NULL);
  ASSERT(r == 0);
  uv_fs_req_cleanup(&fdatasync_req);

  r = uv_fs_close(loop, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  uv_fs_req_cleanup(&close_req);
}

TEST_IMPL(fs_file_noent) {
  uv_fs_t req;
  int r;

  loop = uv_default_loop();

  r = uv_fs_open(NULL, &req, "does_not_exist", O_RDONLY, 0, NULL);
  ASSERT(r == UV_ENOENT);
  ASSERT(req.result == UV_ENOENT);
  uv_fs_req_cleanup(&req);

  /* TODO add EACCES test */

  MAKE_VALGRIND_HAPPY();
  return 0;
}

TEST_IMPL(fs_file_nametoolong) {
  uv_fs_t req;
  int r;
  char name[TOO_LONG_NAME_LENGTH + 1];

  loop = uv_default_loop();

  memset(name, 'a', TOO_LONG_NAME_LENGTH);
  name[TOO_LONG_NAME_LENGTH] = 0;

  r = uv_fs_open(NULL, &req, name, O_RDONLY, 0, NULL);
  ASSERT(r == UV_ENAMETOOLONG);
  ASSERT(req.result == UV_ENAMETOOLONG);
  uv_fs_req_cleanup(&req);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

TEST_IMPL(fs_file_loop) {
  uv_fs_t req;
  int r;

  loop = uv_default_loop();

  unlink("test_symlink");
  r = uv_fs_symlink(NULL, &req, "test_symlink", "test_symlink", 0, NULL);
#ifdef _WIN32
  /*
   * Windows XP and Server 2003 don't support symlinks; we'll get UV_ENOTSUP.
   * Starting with vista they are supported, but only when elevated, otherwise
   * we'll see UV_EPERM.
   */
  if (r == UV_ENOTSUP || r == UV_EPERM) return 0;
#elif defined(__MSYS__)
  /* MSYS2's approximation of symlinks with copies does not work for broken
     links.  */
  if (r == UV_ENOENT) return 0;
#endif
  ASSERT(r == 0);
  uv_fs_req_cleanup(&req);

  r = uv_fs_open(NULL, &req, "test_symlink", O_RDONLY, 0, NULL);
  ASSERT(r == UV_ELOOP);
  ASSERT(req.result == UV_ELOOP);
  uv_fs_req_cleanup(&req);

  unlink("test_symlink");

  MAKE_VALGRIND_HAPPY();
  return 0;
}

TEST_IMPL(fs_file_async) {
  int r;

  /* Setup. */
  unlink("test_file");
  unlink("test_file2");

  loop = uv_default_loop();

  // TODO(chengzhong.wcz): async open
  r = uv_fs_open(loop,
                 &open_req1,
                 "test_file",
                 O_WRONLY | O_CREAT,
                 S_IRUSR | S_IWUSR,
                 NULL);
  ASSERT(r >= 0);
  iov = uv_buf_init(test_buf, sizeof(test_buf));
  r = uv_fs_write(loop, &write_req, r, &iov, 1, -1, write_cb);
  ASSERT(r == 0);
  uv_run(loop, UV_RUN_DEFAULT);

  ASSERT(write_cb_count == 1);
  // open_req1.result is been used in write_cb;
  uv_fs_req_cleanup(&open_req1);

  // TODO(chengzhong.wcz): async open
  r = uv_fs_open(loop,
                 &open_req1,
                 "test_file",
                 O_RDWR,
                 0,
                 NULL);
  ASSERT(r >= 0);
  uv_fs_req_cleanup(&open_req1);
  memset(buf, 0, sizeof(buf));
  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(loop, &read_req, open_req1.result, &iov, 1, -1,
      read_cb);
  ASSERT(r == 0);

  uv_run(loop, UV_RUN_DEFAULT);
  ASSERT(read_cb_count == 1);
  // open_req1.result is been used in read_cb;
  uv_fs_req_cleanup(&open_req1);

  // TODO(chengzhong.wcz): async open
  r = uv_fs_open(loop,
                 &open_req1,
                 "test_file",
                 O_RDONLY,
                 0,
                 NULL);
  memset(buf, 0, sizeof(buf));
  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(loop, &read_req, open_req1.result, &iov, 1, -1,
      read_cb);
  ASSERT(r == 0);

  uv_run(loop, UV_RUN_DEFAULT);
  ASSERT(read_cb_count == 2);
  ASSERT(write_cb_count == 1);

  /* Cleanup. */
  unlink("test_file");

  MAKE_VALGRIND_HAPPY();
  return 0;
}

static void fs_file_sync(int add_flags) {
  int r;

  /* Setup. */
  unlink("test_file");
  unlink("test_file2");

  loop = uv_default_loop();

  r = uv_fs_open(loop,
                 &open_req1,
                 "test_file",
                 O_WRONLY | O_CREAT | add_flags,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  iov = uv_buf_init(test_buf, sizeof(test_buf));
  r = uv_fs_write(NULL, &write_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r >= 0);
  ASSERT(write_req.result >= 0);
  uv_fs_req_cleanup(&write_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  r = uv_fs_open(NULL, &open_req1, "test_file", O_RDWR | add_flags, 0, NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL, &read_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r >= 0);
  ASSERT(read_req.result >= 0);
  ASSERT(strcmp(buf, test_buf) == 0);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_ftruncate(NULL, &ftruncate_req, open_req1.result, 7, NULL);
  ASSERT(r == 0);
  ASSERT(ftruncate_req.result == 0);
  uv_fs_req_cleanup(&ftruncate_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  r = uv_fs_rename(NULL, &rename_req, "test_file", "test_file2", NULL);
  ASSERT(r == 0);
  ASSERT(rename_req.result == 0);
  uv_fs_req_cleanup(&rename_req);

  r = uv_fs_open(
      NULL, &open_req1, "test_file2", O_RDONLY | add_flags, 0, NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  memset(buf, 0, sizeof(buf));
  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL, &read_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r >= 0);
  ASSERT(read_req.result >= 0);
  ASSERT(strcmp(buf, "test-bu") == 0);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  r = uv_fs_unlink(NULL, &unlink_req, "test_file2", NULL);
  ASSERT(r == 0);
  ASSERT(unlink_req.result == 0);
  uv_fs_req_cleanup(&unlink_req);

  /* Cleanup */
  unlink("test_file");
  unlink("test_file2");
}

TEST_IMPL(fs_file_sync) {
  fs_file_sync(0);
  fs_file_sync(UV_FS_O_FILEMAP);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

static void fs_file_write_null_buffer(int add_flags) {
  int r;

  /* Setup. */
  unlink("test_file");

  loop = uv_default_loop();

  r = uv_fs_open(NULL,
                 &open_req1,
                 "test_file",
                 O_WRONLY | O_CREAT | add_flags,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  iov = uv_buf_init(NULL, 0);
  r = uv_fs_write(NULL, &write_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r == 0);
  ASSERT(write_req.result == 0);
  uv_fs_req_cleanup(&write_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  unlink("test_file");
}
TEST_IMPL(fs_file_write_null_buffer) {
  fs_file_write_null_buffer(0);
  fs_file_write_null_buffer(UV_FS_O_FILEMAP);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

static void fs_file_open_append(int add_flags) {
  int r;

  /* Setup. */
  unlink("test_file");

  loop = uv_default_loop();

  r = uv_fs_open(NULL,
                 &open_req1,
                 "test_file",
                 O_WRONLY | O_CREAT | add_flags,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  iov = uv_buf_init(test_buf, sizeof(test_buf));
  r = uv_fs_write(NULL, &write_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r >= 0);
  ASSERT(write_req.result >= 0);
  uv_fs_req_cleanup(&write_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  r = uv_fs_open(
      NULL, &open_req1, "test_file", O_RDWR | O_APPEND | add_flags, 0, NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  iov = uv_buf_init(test_buf, sizeof(test_buf));
  r = uv_fs_write(NULL, &write_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r >= 0);
  ASSERT(write_req.result >= 0);
  uv_fs_req_cleanup(&write_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  r = uv_fs_open(
      NULL, &open_req1, "test_file", O_RDONLY | add_flags, S_IRUSR, NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL, &read_req, open_req1.result, &iov, 1, -1, NULL);
  printf("read = %d\n", r);
  ASSERT(r == 26);
  ASSERT(read_req.result == 26);
  ASSERT(memcmp(buf,
                "test-buffer\n\0test-buffer\n\0",
                sizeof("test-buffer\n\0test-buffer\n\0") - 1) == 0);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  /* Cleanup */
  unlink("test_file");
}
TEST_IMPL(fs_file_open_append) {
  fs_file_open_append(0);
  fs_file_open_append(UV_FS_O_FILEMAP);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

static void fs_read_bufs(int add_flags) {
  char scratch[768];
  uv_buf_t bufs[4];

  ASSERT(0 <= uv_fs_open(NULL,
                         &open_req1,
                         "test/fixtures/lorem_ipsum.txt",
                         O_RDONLY | add_flags,
                         0,
                         NULL));
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  ASSERT(UV_EINVAL ==
         uv_fs_read(NULL, &read_req, open_req1.result, NULL, 0, 0, NULL));
  ASSERT(UV_EINVAL ==
         uv_fs_read(NULL, &read_req, open_req1.result, NULL, 1, 0, NULL));
  ASSERT(UV_EINVAL ==
         uv_fs_read(NULL, &read_req, open_req1.result, bufs, 0, 0, NULL));

  bufs[0] = uv_buf_init(scratch + 0, 256);
  bufs[1] = uv_buf_init(scratch + 256, 256);
  bufs[2] = uv_buf_init(scratch + 512, 128);
  bufs[3] = uv_buf_init(scratch + 640, 128);

  ASSERT(446 == uv_fs_read(NULL,
                           &read_req,
                           open_req1.result,
                           bufs + 0,
                           2, /* 2x 256 bytes. */
                           0, /* Positional read. */
                           NULL));
  ASSERT(read_req.result == 446);
  uv_fs_req_cleanup(&read_req);

  ASSERT(190 == uv_fs_read(NULL,
                           &read_req,
                           open_req1.result,
                           bufs + 2,
                           2,   /* 2x 128 bytes. */
                           256, /* Positional read. */
                           NULL));
  ASSERT(read_req.result == /* 446 - 256 */ 190);
  uv_fs_req_cleanup(&read_req);

  ASSERT(0 == memcmp(bufs[1].base + 0, bufs[2].base, 128));
  ASSERT(0 == memcmp(bufs[1].base + 128, bufs[3].base, 190 - 128));

  ASSERT(0 == uv_fs_close(NULL, &close_req, open_req1.result, NULL));
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);
}
TEST_IMPL(fs_read_bufs) {
  fs_read_bufs(0);
  fs_read_bufs(UV_FS_O_FILEMAP);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

static void fs_read_file_eof(int add_flags) {
#if defined(__CYGWIN__) || defined(__MSYS__)
  RETURN_SKIP("Cygwin pread at EOF may (incorrectly) return data!");
#endif
  int r;

  /* Setup. */
  unlink("test_file");

  loop = uv_default_loop();

  r = uv_fs_open(NULL,
                 &open_req1,
                 "test_file",
                 O_WRONLY | O_CREAT | add_flags,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  iov = uv_buf_init(test_buf, sizeof(test_buf));
  r = uv_fs_write(NULL, &write_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r >= 0);
  ASSERT(write_req.result >= 0);
  uv_fs_req_cleanup(&write_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  r = uv_fs_open(NULL, &open_req1, "test_file", O_RDONLY | add_flags, 0, NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  memset(buf, 0, sizeof(buf));
  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL, &read_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r >= 0);
  ASSERT(read_req.result >= 0);
  ASSERT(strcmp(buf, test_buf) == 0);
  uv_fs_req_cleanup(&read_req);

  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(
      NULL, &read_req, open_req1.result, &iov, 1, read_req.result, NULL);
  ASSERT(r == 0);
  ASSERT(read_req.result == 0);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  /* Cleanup */
  unlink("test_file");
}
TEST_IMPL(fs_read_file_eof) {
  fs_read_file_eof(0);
  fs_read_file_eof(UV_FS_O_FILEMAP);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

static void fs_write_multiple_bufs(int add_flags) {
  uv_buf_t iovs[2];
  int r;

  /* Setup. */
  unlink("test_file");

  loop = uv_default_loop();

  r = uv_fs_open(NULL,
                 &open_req1,
                 "test_file",
                 O_WRONLY | O_CREAT | add_flags,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  iovs[0] = uv_buf_init(test_buf, sizeof(test_buf));
  iovs[1] = uv_buf_init(test_buf2, sizeof(test_buf2));
  r = uv_fs_write(NULL, &write_req, open_req1.result, iovs, 2, 0, NULL);
  ASSERT(r >= 0);
  ASSERT(write_req.result >= 0);
  uv_fs_req_cleanup(&write_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  r = uv_fs_open(NULL, &open_req1, "test_file", O_RDONLY | add_flags, 0, NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  memset(buf, 0, sizeof(buf));
  memset(buf2, 0, sizeof(buf2));
  /* Read the strings back to separate buffers. */
  iovs[0] = uv_buf_init(buf, sizeof(test_buf));
  iovs[1] = uv_buf_init(buf2, sizeof(test_buf2));
  ASSERT(lseek(open_req1.result, 0, SEEK_CUR) == 0);
  r = uv_fs_read(NULL, &read_req, open_req1.result, iovs, 2, -1, NULL);
  ASSERT(r >= 0);
  ASSERT(read_req.result == sizeof(test_buf) + sizeof(test_buf2));
  ASSERT(strcmp(buf, test_buf) == 0);
  ASSERT(strcmp(buf2, test_buf2) == 0);
  uv_fs_req_cleanup(&read_req);

  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL, &read_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r == 0);
  ASSERT(read_req.result == 0);
  uv_fs_req_cleanup(&read_req);

  /* Read the strings back to separate buffers. */
  iovs[0] = uv_buf_init(buf, sizeof(test_buf));
  iovs[1] = uv_buf_init(buf2, sizeof(test_buf2));
  r = uv_fs_read(NULL, &read_req, open_req1.result, iovs, 2, 0, NULL);
  ASSERT(r >= 0);
  if (read_req.result == sizeof(test_buf)) {
    /* Infer that preadv is not available. */
    uv_fs_req_cleanup(&read_req);
    r = uv_fs_read(
        NULL, &read_req, open_req1.result, &iovs[1], 1, read_req.result, NULL);
    ASSERT(r >= 0);
    ASSERT(read_req.result == sizeof(test_buf2));
  } else {
    ASSERT(read_req.result == sizeof(test_buf) + sizeof(test_buf2));
  }
  ASSERT(strcmp(buf, test_buf) == 0);
  ASSERT(strcmp(buf2, test_buf2) == 0);
  uv_fs_req_cleanup(&read_req);

  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL,
                 &read_req,
                 open_req1.result,
                 &iov,
                 1,
                 sizeof(test_buf) + sizeof(test_buf2),
                 NULL);
  ASSERT(r == 0);
  ASSERT(read_req.result == 0);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  /* Cleanup */
  unlink("test_file");
}
TEST_IMPL(fs_write_multiple_bufs) {
  fs_write_multiple_bufs(0);
  fs_write_multiple_bufs(UV_FS_O_FILEMAP);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

static void fs_write_alotof_bufs(int add_flags) {
  size_t iovcount;
  size_t iovmax;
  uv_buf_t* iovs;
  char* buffer;
  size_t index;
  int r;

  iovcount = 54321;

  /* Setup. */
  unlink("test_file");

  loop = uv_default_loop();

  iovs = malloc(sizeof(*iovs) * iovcount);
  ASSERT(iovs != NULL);
  iovmax = uv_test_getiovmax();

  r = uv_fs_open(NULL,
                 &open_req1,
                 "test_file",
                 O_RDWR | O_CREAT | add_flags,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  for (index = 0; index < iovcount; ++index)
    iovs[index] = uv_buf_init(test_buf, sizeof(test_buf));

  r = uv_fs_write(
      NULL, &write_req, open_req1.result, iovs, iovcount, -1, NULL);
  ASSERT(r >= 0);
  ASSERT((size_t)write_req.result == sizeof(test_buf) * iovcount);
  uv_fs_req_cleanup(&write_req);

  /* Read the strings back to separate buffers. */
  buffer = malloc(sizeof(test_buf) * iovcount);
  ASSERT(buffer != NULL);

  for (index = 0; index < iovcount; ++index)
    iovs[index] =
        uv_buf_init(buffer + index * sizeof(test_buf), sizeof(test_buf));

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  r = uv_fs_open(NULL, &open_req1, "test_file", O_RDONLY | add_flags, 0, NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  r = uv_fs_read(NULL, &read_req, open_req1.result, iovs, iovcount, -1, NULL);
  if (iovcount > iovmax) iovcount = iovmax;
  ASSERT(r >= 0);
  ASSERT((size_t)read_req.result == sizeof(test_buf) * iovcount);

  for (index = 0; index < iovcount; ++index)
    ASSERT(strncmp(buffer + index * sizeof(test_buf),
                   test_buf,
                   sizeof(test_buf)) == 0);

  uv_fs_req_cleanup(&read_req);
  free(buffer);

  ASSERT(lseek(open_req1.result, write_req.result, SEEK_SET) ==
         write_req.result);
  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL, &read_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r == 0);
  ASSERT(read_req.result == 0);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  /* Cleanup */
  unlink("test_file");
  free(iovs);
}
TEST_IMPL(fs_write_alotof_bufs) {
  fs_write_alotof_bufs(0);
  fs_write_alotof_bufs(UV_FS_O_FILEMAP);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

static void fs_write_alotof_bufs_async_write_cb(uv_fs_t* req) {
  int r;
  ASSERT(req == &write_req);
  ASSERT(req->fs_type == UV_FS_WRITE);
  ASSERT(req->result >= 0);
  fs_write_alotof_bufs_async_cb_count++;
  uv_fs_req_cleanup(req);

  r = uv_fs_fdatasync(loop, &fdatasync_req, open_req1.result, NULL);
  ASSERT(r == 0);
  uv_fs_req_cleanup(&fdatasync_req);
}
static void fs_write_alotof_bufs_async(int add_flags) {
  size_t iovcount;
  size_t iovmax;
  uv_buf_t* iovs;
  char* buffer;
  size_t index;
  int r;

  iovcount = 54321;

  /* Setup. */
  unlink("test_file");

  loop = uv_default_loop();

  iovs = malloc(sizeof(*iovs) * iovcount);
  ASSERT(iovs != NULL);
  iovmax = uv_test_getiovmax();

  r = uv_fs_open(NULL,
                 &open_req1,
                 "test_file",
                 O_RDWR | O_CREAT | add_flags,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  for (index = 0; index < iovcount; ++index)
    iovs[index] = uv_buf_init(test_buf, sizeof(test_buf));

  int fs_write_alotof_bufs_async_cb_count_snapshot =
      fs_write_alotof_bufs_async_cb_count;
  r = uv_fs_write(loop,
                  &write_req,
                  open_req1.result,
                  iovs,
                  iovcount,
                  -1,
                  fs_write_alotof_bufs_async_write_cb);
  uv_run(loop, UV_RUN_DEFAULT);
  ASSERT(fs_write_alotof_bufs_async_cb_count ==
         fs_write_alotof_bufs_async_cb_count_snapshot + 1);
  ASSERT((size_t)write_req.result == sizeof(test_buf) * iovcount);
  uv_fs_req_cleanup(&write_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  /* Read the strings back to separate buffers. */
  buffer = malloc(sizeof(test_buf) * iovcount);
  ASSERT(buffer != NULL);

  for (index = 0; index < iovcount; ++index)
    iovs[index] =
        uv_buf_init(buffer + index * sizeof(test_buf), sizeof(test_buf));

  r = uv_fs_open(NULL, &open_req1, "test_file", O_RDONLY | add_flags, 0, NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  r = uv_fs_read(NULL, &read_req, open_req1.result, iovs, iovcount, -1, NULL);
  if (iovcount > iovmax) iovcount = iovmax;
  ASSERT(r >= 0);
  ASSERT((size_t)read_req.result == sizeof(test_buf) * iovcount);

  for (index = 0; index < iovcount; ++index)
    ASSERT(strncmp(buffer + index * sizeof(test_buf),
                   test_buf,
                   sizeof(test_buf)) == 0);

  uv_fs_req_cleanup(&read_req);
  free(buffer);

  ASSERT(lseek(open_req1.result, write_req.result, SEEK_SET) ==
         write_req.result);
  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL, &read_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r == 0);
  ASSERT(read_req.result == 0);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  /* Cleanup */
  unlink("test_file");
  free(iovs);
}
TEST_IMPL(fs_write_alotof_bufs_async) {
  fs_write_alotof_bufs_async(0);
  fs_write_alotof_bufs_async(UV_FS_O_FILEMAP);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

static void fs_write_alotof_bufs_with_offset(int add_flags) {
  size_t iovcount;
  size_t iovmax;
  uv_buf_t* iovs;
  char* buffer;
  size_t index;
  int r;
  int64_t offset;
  char* filler;
  int filler_len;

  filler = "0123456789";
  filler_len = strlen(filler);
  iovcount = 54321;

  /* Setup. */
  unlink("test_file");

  loop = uv_default_loop();

  iovs = malloc(sizeof(*iovs) * iovcount);
  ASSERT(iovs != NULL);
  iovmax = uv_test_getiovmax();

  r = uv_fs_open(NULL,
                 &open_req1,
                 "test_file",
                 O_RDWR | O_CREAT | add_flags,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  iov = uv_buf_init(filler, filler_len);
  r = uv_fs_write(NULL, &write_req, open_req1.result, &iov, 1, -1, NULL);
  ASSERT(r == filler_len);
  ASSERT(write_req.result == filler_len);
  uv_fs_req_cleanup(&write_req);
  offset = (int64_t)r;

  for (index = 0; index < iovcount; ++index)
    iovs[index] = uv_buf_init(test_buf, sizeof(test_buf));

  r = uv_fs_write(
      NULL, &write_req, open_req1.result, iovs, iovcount, offset, NULL);
  ASSERT(r >= 0);
  ASSERT((size_t)write_req.result == sizeof(test_buf) * iovcount);
  uv_fs_req_cleanup(&write_req);

  /* Read the strings back to separate buffers. */
  buffer = malloc(sizeof(test_buf) * iovcount);
  ASSERT(buffer != NULL);

  for (index = 0; index < iovcount; ++index)
    iovs[index] =
        uv_buf_init(buffer + index * sizeof(test_buf), sizeof(test_buf));

  r = uv_fs_read(
      NULL, &read_req, open_req1.result, iovs, iovcount, offset, NULL);
  ASSERT(r >= 0);
  if (r == sizeof(test_buf))
    iovcount = 1; /* Infer that preadv is not available. */
  else if (iovcount > iovmax)
    iovcount = iovmax;
  ASSERT((size_t)read_req.result == sizeof(test_buf) * iovcount);

  for (index = 0; index < iovcount; ++index)
    ASSERT(strncmp(buffer + index * sizeof(test_buf),
                   test_buf,
                   sizeof(test_buf)) == 0);

  uv_fs_req_cleanup(&read_req);
  free(buffer);

  r = uv_fs_stat(NULL, &stat_req, "test_file", NULL);
  ASSERT(r == 0);
  ASSERT((int64_t)((uv_stat_t*)stat_req.ptr)->st_size ==
         offset + (int64_t)write_req.result);
  uv_fs_req_cleanup(&stat_req);

  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL,
                 &read_req,
                 open_req1.result,
                 &iov,
                 1,
                 offset + write_req.result,
                 NULL);
  ASSERT(r == 0);
  ASSERT(read_req.result == 0);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  /* Cleanup */
  unlink("test_file");
  free(iovs);
}
TEST_IMPL(fs_write_alotof_bufs_with_offset) {
  fs_write_alotof_bufs_with_offset(0);
  fs_write_alotof_bufs_with_offset(UV_FS_O_FILEMAP);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

#ifdef _WIN32

TEST_IMPL(fs_partial_read) {
  RETURN_SKIP("Test not implemented on Windows.");
}

TEST_IMPL(fs_partial_write) {
  RETURN_SKIP("Test not implemented on Windows.");
}

#else /* !_WIN32 */

struct thread_ctx {
  pthread_t pid;
  int fd;
  char* data;
  int size;
  int interval;
  int doread;
};

static void thread_main(void* arg) {
  const struct thread_ctx* ctx;
  int size;
  char* data;

  ctx = (struct thread_ctx*)arg;
  size = ctx->size;
  data = ctx->data;

  while (size > 0) {
    ssize_t result;
    int nbytes;
    nbytes = size < ctx->interval ? size : ctx->interval;
    if (ctx->doread) {
      result = write(ctx->fd, data, nbytes);
      /* Should not see EINTR (or other errors) */
      ASSERT(result == nbytes);
    } else {
      result = read(ctx->fd, data, nbytes);
      /* Should not see EINTR (or other errors),
       * but might get a partial read if we are faster than the writer
       */
      ASSERT(result > 0 && result <= nbytes);
    }

    pthread_kill(ctx->pid, SIGUSR1);
    size -= result;
    data += result;
  }
}

static void sig_func(uv_signal_t* handle, int signum) {
  uv_signal_stop(handle);
}

static size_t uv_test_fs_buf_offset(uv_buf_t* bufs, size_t size) {
  size_t offset;
  /* Figure out which bufs are done */
  for (offset = 0; size > 0 && bufs[offset].len <= size; ++offset)
    size -= bufs[offset].len;

  /* Fix a partial read/write */
  if (size > 0) {
    bufs[offset].base += size;
    bufs[offset].len -= size;
  }
  return offset;
}

static void test_fs_partial(int doread) {
  struct thread_ctx ctx;
  uv_thread_t thread;
  uv_signal_t signal;
  int pipe_fds[2];
  size_t iovcount;
  uv_buf_t* iovs;
  char* buffer;
  size_t index;

  iovcount = 54321;

  iovs = malloc(sizeof(*iovs) * iovcount);
  ASSERT(iovs != NULL);

  ctx.pid = pthread_self();
  ctx.doread = doread;
  ctx.interval = 1000;
  ctx.size = sizeof(test_buf) * iovcount;
  ctx.data = malloc(ctx.size);
  ASSERT(ctx.data != NULL);
  buffer = malloc(ctx.size);
  ASSERT(buffer != NULL);

  for (index = 0; index < iovcount; ++index)
    iovs[index] =
        uv_buf_init(buffer + index * sizeof(test_buf), sizeof(test_buf));

  loop = uv_default_loop();

  ASSERT(0 == uv_signal_init(loop, &signal));
  ASSERT(0 == uv_signal_start(&signal, sig_func, SIGUSR1));

  ASSERT(0 == pipe(pipe_fds));

  ctx.fd = pipe_fds[doread];
  ASSERT(0 == uv_thread_create(&thread, thread_main, &ctx));

  if (doread) {
    uv_buf_t* read_iovs;
    int nread;
    read_iovs = iovs;
    nread = 0;
    while (nread < ctx.size) {
      int result;
      result = uv_fs_read(
          loop, &read_req, pipe_fds[0], read_iovs, iovcount, -1, NULL);
      if (result > 0) {
        size_t read_iovcount;
        read_iovcount = uv_test_fs_buf_offset(read_iovs, result);
        read_iovs += read_iovcount;
        iovcount -= read_iovcount;
        nread += result;
      } else {
        ASSERT(result == UV_EINTR);
      }
      uv_fs_req_cleanup(&read_req);
    }
  } else {
    int result;
    result =
        uv_fs_write(loop, &write_req, pipe_fds[1], iovs, iovcount, -1, NULL);
    ASSERT(write_req.result == result);
    ASSERT(result == ctx.size);
    uv_fs_req_cleanup(&write_req);
  }

  ASSERT(0 == memcmp(buffer, ctx.data, ctx.size));

  ASSERT(0 == uv_thread_join(&thread));
  ASSERT(0 == uv_run(loop, UV_RUN_DEFAULT));

  ASSERT(0 == close(pipe_fds[1]));
  uv_close((uv_handle_t*)&signal, NULL);

  { /* Make sure we read everything that we wrote. */
    int result;
    result = uv_fs_read(loop, &read_req, pipe_fds[0], iovs, 1, -1, NULL);
    ASSERT(result == 0);
    uv_fs_req_cleanup(&read_req);
  }
  ASSERT(0 == close(pipe_fds[0]));

  free(iovs);
  free(buffer);
  free(ctx.data);

  MAKE_VALGRIND_HAPPY();
}

TEST_IMPL(fs_partial_read) {
  test_fs_partial(1);
  return 0;
}

TEST_IMPL(fs_partial_write) {
  test_fs_partial(0);
  return 0;
}

#endif /* _WIN32 */

TEST_IMPL(fs_read_write_null_arguments) {
  int r;

  r = uv_fs_read(NULL, &read_req, 0, NULL, 0, -1, NULL);
  ASSERT(r == UV_EINVAL);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_write(NULL, &write_req, 0, NULL, 0, -1, NULL);
  /* Validate some memory management on failed input validation before sending
     fs work to the thread pool. */
  ASSERT(r == UV_EINVAL);
  ASSERT(write_req.path == NULL);
  ASSERT(write_req.ptr == NULL);
#ifdef _WIN32
  ASSERT(write_req.file.pathw == NULL);
  ASSERT(write_req.fs.info.new_pathw == NULL);
  ASSERT(write_req.fs.info.bufs == NULL);
#else
  ASSERT(write_req.new_path == NULL);
  ASSERT(write_req.bufs == NULL);
#endif
  uv_fs_req_cleanup(&write_req);

  iov = uv_buf_init(NULL, 0);
  r = uv_fs_read(NULL, &read_req, 0, &iov, 0, -1, NULL);
  ASSERT(r == UV_EINVAL);
  uv_fs_req_cleanup(&read_req);

  iov = uv_buf_init(NULL, 0);
  r = uv_fs_write(NULL, &write_req, 0, &iov, 0, -1, NULL);
  ASSERT(r == UV_EINVAL);
  uv_fs_req_cleanup(&write_req);

  /* If the arguments are invalid, the loop should not be kept open */
  loop = uv_default_loop();

  r = uv_fs_read(loop, &read_req, 0, NULL, 0, -1, fail_cb);
  ASSERT(r == UV_EINVAL);
  uv_run(loop, UV_RUN_DEFAULT);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_write(loop, &write_req, 0, NULL, 0, -1, fail_cb);
  ASSERT(r == UV_EINVAL);
  uv_run(loop, UV_RUN_DEFAULT);
  uv_fs_req_cleanup(&write_req);

  iov = uv_buf_init(NULL, 0);
  r = uv_fs_read(loop, &read_req, 0, &iov, 0, -1, fail_cb);
  ASSERT(r == UV_EINVAL);
  uv_run(loop, UV_RUN_DEFAULT);
  uv_fs_req_cleanup(&read_req);

  iov = uv_buf_init(NULL, 0);
  r = uv_fs_write(loop, &write_req, 0, &iov, 0, -1, fail_cb);
  ASSERT(r == UV_EINVAL);
  uv_run(loop, UV_RUN_DEFAULT);
  uv_fs_req_cleanup(&write_req);

  return 0;
}

TEST_IMPL(get_osfhandle_valid_handle) {
  int r;
  uv_os_fd_t fd;

  /* Setup. */
  unlink("test_file");

  loop = uv_default_loop();

  r = uv_fs_open(NULL,
                 &open_req1,
                 "test_file",
                 O_RDWR | O_CREAT,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  fd = uv_get_osfhandle(open_req1.result);
#ifdef _WIN32
  ASSERT(fd != INVALID_HANDLE_VALUE);
#else
  ASSERT(fd >= 0);
#endif

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  /* Cleanup. */
  unlink("test_file");

  MAKE_VALGRIND_HAPPY();
  return 0;
}

TEST_IMPL(open_osfhandle_valid_handle) {
  int r;
  uv_os_fd_t handle;
  int fd;

  /* Setup. */
  unlink("test_file");

  loop = uv_default_loop();

  r = uv_fs_open(NULL,
                 &open_req1,
                 "test_file",
                 O_RDWR | O_CREAT,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r >= 0);
  ASSERT(open_req1.result >= 0);
  uv_fs_req_cleanup(&open_req1);

  handle = uv_get_osfhandle(open_req1.result);
#ifdef _WIN32
  ASSERT(handle != INVALID_HANDLE_VALUE);
#else
  ASSERT(handle >= 0);
#endif

  fd = uv_open_osfhandle(handle);
#ifdef _WIN32
  ASSERT(fd > 0);
#else
  ASSERT(fd == open_req1.result);
#endif

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  ASSERT(close_req.result == 0);
  uv_fs_req_cleanup(&close_req);

  /* Cleanup. */
  unlink("test_file");

  MAKE_VALGRIND_HAPPY();
  return 0;
}

TEST_IMPL(fs_file_pos_after_op_with_offset) {
  int r;

  /* Setup. */
  unlink("test_file");
  loop = uv_default_loop();

  r = uv_fs_open(loop,
                 &open_req1,
                 "test_file",
                 O_RDWR | O_CREAT,
                 S_IWUSR | S_IRUSR,
                 NULL);
  ASSERT(r > 0);
  uv_fs_req_cleanup(&open_req1);

  iov = uv_buf_init(test_buf, sizeof(test_buf));
  r = uv_fs_write(NULL, &write_req, open_req1.result, &iov, 1, 0, NULL);
  ASSERT(r == sizeof(test_buf));
  ASSERT(lseek(open_req1.result, 0, SEEK_CUR) == 0);
  uv_fs_req_cleanup(&write_req);

  iov = uv_buf_init(buf, sizeof(buf));
  r = uv_fs_read(NULL, &read_req, open_req1.result, &iov, 1, 0, NULL);
  ASSERT(r == sizeof(test_buf));
  ASSERT(strcmp(buf, test_buf) == 0);
  ASSERT(lseek(open_req1.result, 0, SEEK_CUR) == 0);
  uv_fs_req_cleanup(&read_req);

  r = uv_fs_close(NULL, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  uv_fs_req_cleanup(&close_req);

  /* Cleanup */
  unlink("test_file");

  MAKE_VALGRIND_HAPPY();
  return 0;
}

TEST_IMPL(fs_null_req) {
  /* Verify that all fs functions return UV_EINVAL when the request is NULL. */
  int r;

  r = uv_fs_open(NULL, NULL, NULL, 0, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_close(NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_read(NULL, NULL, 0, NULL, 0, -1, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_write(NULL, NULL, 0, NULL, 0, -1, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_unlink(NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_mkdir(NULL, NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_mkdtemp(NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_mkstemp(NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_rmdir(NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_scandir(NULL, NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_link(NULL, NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_symlink(NULL, NULL, NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_readlink(NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_realpath(NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_chown(NULL, NULL, NULL, 0, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_fchown(NULL, NULL, 0, 0, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_stat(NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_lstat(NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_fstat(NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_rename(NULL, NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_fsync(NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_fdatasync(NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_ftruncate(NULL, NULL, 0, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_copyfile(NULL, NULL, NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_sendfile(NULL, NULL, 0, 0, 0, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_access(NULL, NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_chmod(NULL, NULL, NULL, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_fchmod(NULL, NULL, 0, 0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_utime(NULL, NULL, NULL, 0.0, 0.0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_futime(NULL, NULL, 0, 0.0, 0.0, NULL);
  ASSERT(r == UV_EINVAL);

  r = uv_fs_statfs(NULL, NULL, NULL, NULL);
  ASSERT(r == UV_EINVAL);

  /* This should be a no-op. */
  uv_fs_req_cleanup(NULL);

  return 0;
}

TEST_IMPL(fs_get_system_error) {
  uv_fs_t req;
  int r;
  int system_error;

  r = uv_fs_statfs(NULL, &req, "non_existing_file", NULL);
  ASSERT(r != 0);

  system_error = uv_fs_get_system_error(&req);
#ifdef _WIN32
  ASSERT(system_error == ERROR_FILE_NOT_FOUND);
#else
  ASSERT(system_error == ENOENT);
#endif

  return 0;
}

TEST_IMPL(fs_read) {
  return 0;
}
