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

#include "uv.h"
#include "internal.h"
#include "heap-inl.h"
#include "list.h"

#include <assert.h>
#include <limits.h>

int uv__init_timers(uv_loop_t* loop)
{
  int j;
  struct tvec_base *base = &loop->vec_base;
  
  for (j = 0; j < TVN_SIZE; j++) {
    INIT_LIST_HEAD (base->tv5.vec + j);
    INIT_LIST_HEAD (base->tv4.vec + j);
    INIT_LIST_HEAD (base->tv3.vec + j);
    INIT_LIST_HEAD (base->tv2.vec + j);
  }

  for (j = 0; j < TVR_SIZE; j++) {
    INIT_LIST_HEAD (base->tv1.vec + j);
  }

  base->next_tick = 1;
  base->now_ = loop->time - 1;
  return 0;
}

void add_timer(struct tvec_base *base, uv_timer_t *timer)
{
  int i;
  long idx;
  long expires;
  struct list_head *vec;
  // 1ms
  idx = (timer->timeout - base->now_);
  //if (timer->timeout == (uint64_t)-1 && (signed long) idx < 0) assert(0);
  if ((unsigned long)idx > LONG_MAX) idx = LONG_MAX;

  expires = idx + base->next_tick;

  if (idx < TVR_SIZE) {
    i = expires & TVR_MASK;
    vec = base->tv1.vec + i;
  } else if (idx < 1 << (TVR_BITS + TVN_BITS)) {
    i = (expires >> TVR_BITS) & TVN_MASK;
    vec = base->tv2.vec + i;
  } else if (idx < 1 << (TVR_BITS + 2*TVN_BITS)) {
    i = (expires >> (TVR_BITS + TVN_BITS)) & TVN_MASK;
    vec = base->tv3.vec + i;
  } else if (idx < 1 << (TVR_BITS + 3*TVN_BITS)) {
    i = (expires >> (TVR_BITS + 2*TVN_BITS)) & TVN_MASK;
    vec = base->tv4.vec + i;
  } else if (idx < 0) {
    vec = base->tv1.vec + (base->next_tick & TVR_MASK);
  } else {
     if (idx > MAX_TVAL) {
       idx = MAX_TVAL;
       expires = idx + base->next_tick;
     }
     i = (expires >> (TVR_BITS + 3*TVN_BITS)) & TVN_MASK;
     vec = base->tv5.vec + i;
  }

  list_add_tail(&timer->entry, vec);
}

void detach_timer (uv_timer_t *timer)
{
  struct list_head *entry = &timer->entry;

  list_del(entry);
}

// todo
int cascade(struct tvec_base *base, struct tvec *tv, int index)
{
  uv_timer_t *timer, *tmp;
  struct list_head tv_list;

  list_replace_init (tv->vec + index, &tv_list);

  list_for_each_entry_safe(timer, tmp, &tv_list, entry) {
    add_timer(base, timer);
  }

  return index;
}


int uv_timer_init(uv_loop_t* loop, uv_timer_t* handle) {
  uv__handle_init(loop, (uv_handle_t*)handle, UV_TIMER);
  handle->timer_cb = NULL;
  handle->repeat = 0;
  return 0;
}


int uv_timer_start(uv_timer_t* handle,
                   uv_timer_cb cb,
                   uint64_t timeout,
                   uint64_t repeat) {
  uint64_t clamped_timeout;

  if (cb == NULL)
    return -EINVAL;

  if (uv__is_active(handle))
    uv_timer_stop(handle);

  clamped_timeout = handle->loop->time + timeout;
  if (clamped_timeout < timeout) {
    clamped_timeout = (uint64_t) -1;
  }

  handle->timer_cb = cb;
  handle->timeout = clamped_timeout;
  handle->repeat = repeat;
  /* start_id is the second index to be compared in uv__timer_cmp() */
  handle->start_id = handle->loop->timer_counter++;

  add_timer(&handle->loop->vec_base, handle);
  uv__handle_start(handle);

  return 0;
}


int uv_timer_stop(uv_timer_t* handle) {
  if (!uv__is_active(handle))
    return 0;

  detach_timer(handle);
  uv__handle_stop(handle);

  return 0;
}


int uv_timer_again(uv_timer_t* handle) {
  if (handle->timer_cb == NULL)
    return -EINVAL;

  if (handle->repeat) {
    uv_timer_stop(handle);
    uv_timer_start(handle, handle->timer_cb, handle->repeat, handle->repeat);
  }

  return 0;
}


void uv_timer_set_repeat(uv_timer_t* handle, uint64_t repeat) {
  handle->repeat = repeat;
}


uint64_t uv_timer_get_repeat(const uv_timer_t* handle) {
  return handle->repeat;
}


int uv__next_timeout(const uv_loop_t* loop) {
  //const struct heap_node* heap_node;
  //const uv_timer_t* handle;
  int j;
  uint64_t diff = 0;
  const struct tvec_base *base = &loop->vec_base;
  for (j = 0; j < TVR_SIZE; j++) {
    if (list_empty(&base->tv1.vec[j])) {
       ++diff;
    }
  }
  //printf("timeout %lld\n", diff);
  return diff;
  // heap_node = heap_min((const struct heap*) &loop->timer_heap);
  // if (heap_node == NULL)
  //   return -1; /* block indefinitely */

  // handle = container_of(heap_node, const uv_timer_t, heap_node);
  // if (handle->timeout <= loop->time)
  //   return 0;

  // diff = handle->timeout - loop->time;
  // if (diff > INT_MAX)
  //   diff = INT_MAX;

  // return diff;
}

#define INDEX(N)  ((base->next_tick >> (TVR_BITS + N * TVN_BITS)) & TVN_MASK)

void uv__run_timers(uv_loop_t* loop) {
  unsigned long index;
  uv_timer_t* handle;
  struct list_head work_list;
  struct list_head *head = &work_list;
  uint64_t catchup = uv__hrtime(UV_CLOCK_FAST) / 1000000;
  struct tvec_base *base = &loop->vec_base;
  //printf("%lld, %lld\n", base->now_, catchup);
  while (base->now_ < catchup)
  {
    base->now_ += 1;
    index  = base->next_tick & TVR_MASK;

    if (!index &&
        (!cascade (base, &base->tv2, INDEX(0))) &&
        (!cascade (base, &base->tv3, INDEX(1))) &&
        (!cascade (base, &base->tv4, INDEX(2))))
            cascade (base, &base->tv5, INDEX(3));

    base->next_tick++;
    list_replace_init (base->tv1.vec + index, head);
    while (!list_empty(head)) {
      handle = list_first_entry (head, uv_timer_t, entry);
      if (handle->timeout > catchup) 
        printf("%lld, %lld\n", handle->timeout, catchup);
      uv_timer_stop(handle);
      uv_timer_again(handle);
      handle->timer_cb(handle);
    }
  }
  
}


void uv__timer_close(uv_timer_t* handle) {
  uv_timer_stop(handle);
}
