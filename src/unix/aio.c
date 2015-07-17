#include "uv.h"
#include "internal.h"
#include "atomic-ops.h"

#include <errno.h>
#include <stdio.h>  /* snprintf() */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UV_AIO_MAX (64)


/* -----------------------------------------------------------*/
static void uv__aio_event(uv_loop_t* loop,
                            struct uv__aio* w,
                            unsigned int nevents);

int uv_aio_init(uv_loop_t* loop, uv_aio_t* handle, uv_aio_cb aio_cb) {
  int err;

  err = uv__aio_start(loop, &loop->aio_watcher, uv__aio_event);
  if (err)
    return err;

  uv__handle_init(loop, (uv_handle_t*)handle, UV_AIO);
  handle->aio_cb = aio_cb;

  QUEUE_INSERT_TAIL(&loop->aio_handles, &handle->queue);
  uv__handle_start(handle);

  return 0;
}


void uv__aio_work_done(uv_aio_t* handle, int64_t n) {
  struct timespec tms;
  int i, r;
  struct uv__aio* wa = &handle->loop->aio_watcher;
  struct io_event* events = uv__malloc(sizeof(struct io_event) * n);
  

  while (n > 0) {
    tms.tv_sec = 0;
    tms.tv_nsec = 0;
    r = uv__getevents(wa->ctx, 1, n, events, &tms);
    if (r > 0) {
      for (i = 0; i < r; ++i) {
        uv_fs_t* req = (uv_fs_t*)events[i].data;
        if (!events[i].res2 && req->result >= 0)
          req->result += events[i].res;
        else 
          req->result = events[i].res;

        if (req->cb != NULL && (--req->aio_nr) == 0) {
          uv__req_unregister(req->loop, req);
          uv__free(req->iocbs);
          req->iocbs = NULL;
          req->cb(req);
        }
      }
      n -= r;
    }
  }
  uv__free(events);
}


void uv__aio_close(uv_aio_t* handle) {
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
}


static void uv__aio_event(uv_loop_t* loop,
                            struct uv__aio* w,
                            unsigned int nevents) {
  QUEUE* q;
  uv_aio_t* h;

  QUEUE_FOREACH(q, &loop->aio_handles) {
    h = QUEUE_DATA(q, uv_aio_t, queue);

    if (h->aio_cb == NULL)
      continue;
    h->aio_cb(h, nevents);
  }
}


static void uv__aio_callback(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  struct uv__aio* wa;
  char buf[1024];
  unsigned n;
  ssize_t r;

  n = 0;
  for (;;) {
    r = read(w->fd, buf, sizeof(buf));
    if (r > 0)
      n += r;

    if (r == sizeof(buf))
      continue;

    if (r != -1)
      break;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;

    if (errno == EINTR)
      continue;

    abort();
  }

  wa = container_of(w, struct uv__aio, aio_watcher);

#if defined(__linux__)
  if (wa->wfd == -1) {
    uint64_t val;
    assert(n == sizeof(val));
    memcpy(&val, buf, sizeof(val));  /* Avoid alignment issues. */
    wa->cb(loop, wa, val);
    return;
  }
#endif

  wa->cb(loop, wa, n);
}


int uv_aio_submit(uv_aio_t* handle, uv_fs_t* req) {
  int err;
  unsigned int i ;
  off_t offset = 0;
  struct iocb *iocbp = NULL;
  struct uv__aio* wa = &handle->loop->aio_watcher;
  struct iocb *iocbps[UV_AIO_MAX];
  if (req->nbufs <= 0 || req->nbufs > UV_AIO_MAX) {
    return -EINVAL;
  }

  if (req->off < 0) {
    req->off = 0;
  }
  
  req->iocbs = uv__malloc(req->nbufs * sizeof(struct iocb));
  if (req->iocbs == NULL)
      return -ENOMEM;

  offset = req->off;
  iocbp = req->iocbs;

  for (i = 0; i < req->nbufs; ++i, ++iocbp) {
    iocbps[i] = iocbp;
    memset((void*)iocbp, 0, sizeof(struct iocb));

    iocbp->aio_fildes = req->file;
    switch (req->fs_type) {
      case UV_FS_READ:
        iocbp->aio_lio_opcode = IOCB_CMD_PREAD;
        break;
      case UV_FS_WRITE:
        iocbp->aio_lio_opcode = IOCB_CMD_PWRITE;
        break;
      default:
        UNREACHABLE();
    }
    iocbp->aio_buf = (uint64_t)req->bufs[i].base;
    iocbp->aio_offset = offset;
    iocbp->aio_nbytes = req->bufs[i].len;

    iocbp->aio_flags = IOCB_FLAG_RESFD;
    iocbp->aio_resfd = wa->aio_watcher.fd;

    iocbp->aio_data = (uint64_t)req;
    offset += req->bufs[i].len;
  }
  req->aio_nr = req->nbufs;
  err = uv__submit(wa->ctx, req->aio_nr, iocbps);
  if (err != req->aio_nr) {
      perror("submit");
      abort();
  }
  if (req->bufs != req->bufsml)
    uv__free(req->bufs);
  return 0;
}

void uv__aio_init(struct uv__aio* wa) {
  wa->aio_watcher.fd = -1;
  wa->ctx = 0;
  wa->wfd = -1;
  if (uv__setup(8192, &wa->ctx)) {
    assert(0 && "uv__setup error\n");
    abort();
  }
}


int uv__aio_start(uv_loop_t* loop, struct uv__aio* wa, uv__aio_cb cb) {
  int pipefd[2];
  int err;

  err = uv__async_eventfd();
  if (err >= 0) {
    pipefd[0] = err;
    pipefd[1] = -1;
  }

  if (err < 0) {
    abort();
  }

  uv__io_init(&wa->aio_watcher, uv__aio_callback, pipefd[0]);
  uv__io_start(loop, &wa->aio_watcher, UV__POLLIN);
  wa->wfd = pipefd[1];
  wa->cb = cb;
  return 0;
}


void uv__aio_stop(uv_loop_t* loop, struct uv__aio* wa) {
  if (wa->aio_watcher.fd == -1)
    return;

  if (wa->wfd != -1) {
    if (wa->wfd != wa->aio_watcher.fd)
      uv__close(wa->wfd);
    wa->wfd = -1;
  }

  uv__io_stop(loop, &wa->aio_watcher, UV__POLLIN);
  uv__close(wa->aio_watcher.fd);
  uv__destroy(wa->ctx);
  wa->aio_watcher.fd = -1;
}
