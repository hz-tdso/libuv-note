/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
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

/* This file contains both the uv__async internal infrastructure and the
 * user-facing uv_async_t functions.
 */

#include "uv.h"
#include "internal.h"
#include "atomic-ops.h"

#include <errno.h>
#include <stdio.h>  /* snprintf() */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>  /* sched_yield() */

#ifdef __linux__
#include <sys/eventfd.h>
#endif

static void uv__async_send(uv_loop_t* loop);
static int uv__async_start(uv_loop_t* loop);

// 此函数可能被调用多次(目前只看到在uv_loop_init函数中调用了一次)，
// 导致loop中的handle_queue队列中有多个uv_async_t类型的handle，
// 但是loop只有一个async_io_watcher，所以当async_io_watcher监听的fd上有io事件发生时，
// 无法确定要调哪个uv_async_t类型的handle上的回调函数
int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
  int err;

  // 主要是初始化并启动loop->async_io_watcher，类型是uv__io_t。
  // 初始化其实就是设置async_io_watcher的成员。
  // 启动其实就是将async_io_watcher插入到loop的watcher_queue队列中，
  // 并将async_io_watcher的地址写入loop的watchers指向的数组中，以async_io_watcher中的fd作为索引，找到数组的槽位。
  err = uv__async_start(loop);
  if (err)
    return err;

  // 初始化handle的成员，并将handle插到loop的handle_queue队列中
  uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
  handle->async_cb = async_cb;
  handle->pending = 0;

  // 将handle插到loop的async_handles队列中
  QUEUE_INSERT_TAIL(&loop->async_handles, &handle->queue);
  // 启动handle，其实就是设置handle的start标志位，并将loop的active_handles++
  uv__handle_start(handle);

  return 0;
}


int uv_async_send(uv_async_t* handle) {
  /* Do a cheap read first. */
  if (ACCESS_ONCE(int, handle->pending) != 0)
    return 0;

  /* Tell the other thread we're busy with the handle. */
  // cmpxchgi(int* ptr, int oldval, int newval)的语义：
  // 如果ptr指向的变量等于oldval，则将ptr指向的变量更新为newval，返回旧值；
  // 如果ptr指向的变量不等于oldval，则返回ptr指向的变量的值。
  // 所以此处的if判断的逻辑为：
  // 如果pending不等于0，则执行第85行；
  // 如果pending等于0，则将pending更新为1，继续执行第88行。
  if (cmpxchgi(&handle->pending, 0, 1) != 0)
    return 0;

  /* Wake up the other thread's event loop. */
  uv__async_send(handle->loop);

  /* Tell the other thread we're done. */
  if (cmpxchgi(&handle->pending, 1, 2) != 1)
    abort();

  return 0;
}


/* Only call this from the event loop thread. */
static int uv__async_spin(uv_async_t* handle) {
  int i;
  int rc;

  for (;;) {
    /* 997 is not completely chosen at random. It's a prime number, acyclical
     * by nature, and should therefore hopefully dampen sympathetic resonance.
     */
    for (i = 0; i < 997; i++) {
      /* rc=0 -- handle is not pending.
       * rc=1 -- handle is pending, other thread is still working with it.
       * rc=2 -- handle is pending, other thread is done.
       */
      rc = cmpxchgi(&handle->pending, 2, 0);

      if (rc != 1)
        return rc;

      /* Other thread is busy with this handle, spin until it's done. */
      cpu_relax();
    }

    /* Yield the CPU. We may have preempted the other thread while it's
     * inside the critical section and if it's running on the same CPU
     * as us, we'll just burn CPU cycles until the end of our time slice.
     */
    sched_yield();
  }
}


void uv__async_close(uv_async_t* handle) {
  uv__async_spin(handle);
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
}

// 此函数作为loop的async_io_watcher上的回调函数
// 参数w就是loop的async_io_watcher
static void uv__async_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  char buf[1024];
  ssize_t r;
  QUEUE queue;
  QUEUE* q;
  uv_async_t* h;

  assert(w == &loop->async_io_watcher);

  for (;;) {
    r = read(w->fd, buf, sizeof(buf));

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

  // async_handles的类型为void* async_handles[2]
  // 此宏定义的功能是将async_handles队列上所有的handle移到queue上，并将loop的async_handles清空
  QUEUE_MOVE(&loop->async_handles, &queue);
  // 遍历所有的async_handle
  while (!QUEUE_EMPTY(&queue)) {
    q = QUEUE_HEAD(&queue);
    h = QUEUE_DATA(q, uv_async_t, queue);

    // 每遍历一个，从队列中删除一个，并重新加入到loop的async_handles队列中
    QUEUE_REMOVE(q);
    QUEUE_INSERT_TAIL(&loop->async_handles, q);

    // 此函数是个死循环，等待uv_async_t上的pending状态变为0或2，
    // 0表示uv_async_t没有挂起；1表示uv_async_t虽然挂起了，但是其他线程已经用完了uv_async_t。
    if (0 == uv__async_spin(h))
      continue;  /* Not pending. */

    if (h->async_cb == NULL)
      continue;

    h->async_cb(h);
  }
}


static void uv__async_send(uv_loop_t* loop) {
  const void* buf;
  ssize_t len;
  int fd;
  int r;

  buf = "";
  len = 1;
  fd = loop->async_wfd;

#if defined(__linux__)
  if (fd == -1) {
    static const uint64_t val = 1;
    buf = &val;
    len = sizeof(val);
    fd = loop->async_io_watcher.fd;  /* eventfd */
  }
#endif

  do
    // 这里的fd实际上和loop的async_io_watcher中的fd是同一个fd
    // 所以，读写的其实是同一块内核缓冲区
    r = write(fd, buf, len);
  while (r == -1 && errno == EINTR);

  if (r == len)
    return;

  if (r == -1)
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;

  abort();
}


static int uv__async_start(uv_loop_t* loop) {
  int pipefd[2];
  int err;

  if (loop->async_io_watcher.fd != -1)
    return 0;

#ifdef __linux__
  // 创建了一个事件管道，返回了事件管道的fd
  err = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (err < 0)
    return UV__ERR(errno);

  pipefd[0] = err;
  // pipefd[1]存的是写操作时的fd，暂时不指定。
  pipefd[1] = -1;
#else
  err = uv__make_pipe(pipefd, UV_NONBLOCK_PIPE);
  if (err < 0)
    return err;
#endif

  // 在初始化async_io_watcher时会将第235行创建的事件管道的fd赋给async_io_watcher的fd，
  // 即表示从这个事件管道中读取数据。
  uv__io_init(&loop->async_io_watcher, uv__async_io, pipefd[0]);
  uv__io_start(loop, &loop->async_io_watcher, POLLIN);
  // pipefd[1] 为 -1。
  loop->async_wfd = pipefd[1];

  return 0;
}


int uv__async_fork(uv_loop_t* loop) {
  if (loop->async_io_watcher.fd == -1) /* never started */
    return 0;

  uv__async_stop(loop);

  return uv__async_start(loop);
}


void uv__async_stop(uv_loop_t* loop) {
  if (loop->async_io_watcher.fd == -1)
    return;

  if (loop->async_wfd != -1) {
    if (loop->async_wfd != loop->async_io_watcher.fd)
      uv__close(loop->async_wfd);
    loop->async_wfd = -1;
  }

  uv__io_stop(loop, &loop->async_io_watcher, POLLIN);
  uv__close(loop->async_io_watcher.fd);
  loop->async_io_watcher.fd = -1;
}
