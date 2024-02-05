#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ebot.h"

/**
 * error functions 错误处理函数
 */
#define ERR_MAXLINE 2048

static void err_doit(int, const char *, va_list);

static void err_sys_(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  err_doit(1, fmt, ap);
  va_end(ap);
  exit(1);
}

static void err_msg_(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  err_doit(0, fmt, ap);
  va_end(ap);
  return;
}

/**
  * print message and return to caller
  */
static void err_doit(int errnoflag, const char *fmt, va_list ap) {
  int errno_save, n;
  char buf[ERR_MAXLINE + 1];
  errno_save = errno;

#ifdef HAVE_VSNPRINTF
  vsnprintf(buf, ERR_MAXLINE, fmt, ap);
#else
  vsprintf(buf, fmt, ap);
#endif

  n = strlen(buf);
  if (errnoflag) {
    snprintf(buf + n, ERR_MAXLINE - n, ": %s", strerror(errno_save));
  }

  strcat(buf, "\n");
  fflush(stdout);
  fputs(buf, stderr);
  fflush(stderr);
  return;
}
#define __QUOTE(x) # x
#define  _QUOTE(x) __QUOTE(x)

#define err_msg(fmt, ...) do {                                \
  time_t ___t = time(NULL);                                   \
  struct tm* ___dm = localtime(&___t);                        \
  err_msg_("[%02d:%02d:%02d] %s:[" _QUOTE(__LINE__) "]\t%s:"  \
    fmt,                                                      \
    ___dm->tm_hour,                                           \
    ___dm->tm_min,                                            \
    ___dm->tm_sec,                                            \
    __FILE__,                                                 \
    __func__,                                                 \
    ## __VA_ARGS__);                                          \
} while(0) 

#define err_print(fmt, func, ...) do { \
  /* TODO:*/                           \
} while(0)

#ifdef DEBUG
#define err_debug(fmt, ...) err_msg(fmt, ## __VA_ARGS__)
#else
#define err_debug(fmt, ...)
#endif
#define err_exit(fmt, ...)  do {\
  err_msg(fmt, ##__VA_ARGS__);  \
  exit(1);                      \
} while(0)

static void vec_expand(char **data, int *length,
  int *capacity, int memsz) {

  if (*length + 1 > *capacity) {
    *capacity = (*capacity == 0) ? 1 : (*capacity << 1);
    *data = realloc(*data, (size_t)(*capacity * memsz));
  }
}

static void vec_splice(char **data, int *length, 
  int *capacity, int memsz, int start, int count ) {

  (void) capacity;
  memmove(*data + start * memsz, *data + (start + count) * memsz, 
    (*length - start - count) * memsz);
}             

/**
 * vector容器
 */
#define Vec(T) \
  struct { T *data; int length, capacity;}

#define vec_unpack(v) \
  (char**)&(v)->data, &(v)->length, &(v)->capacity, sizeof(*(v)->data)

#define vec_init(v) \
  memset((v), 0, sizeof(*(v)))

#define vec_deinit(v) \
  free((v)->data)

#define vec_clear(v) \
  ((v)->length = 0)

#define vec_push(v, val) \
  (vec_expand(vec_unpack(v)), \
  (v)->data[(v)->length++] = (val) )

#define vec_pop(v) \
  (assert((v)->length > 0), \
  (v)->data[-- (v)->length])

#define vec_get(v, pos) \
  (assert(pos < (v)->length), \
  (v)->data[pos])

#define vec_splice(v, start, count)\
  (vec_splice(vec_unpack(v), start, count), \
  (v)->length -= (count))

/**
 * ring queue 环形队列
 */
typedef struct {
  int length;   //buf长度
  char data[0]; //buf数据
} ring_item;

typedef struct {
  int head;
  int tail;
  int size;
  int num;
  int use_lock;
  void *mem;
  pthread_mutex_t lock;
  pthread_mutexattr_t attr;
} ringq;

#ifndef RQ_SIZE
#define RQ_SIZE 1024 * 4
#endif

int ringq_empty(ringq *rq) {
  return rq->tail == rq->head ? 0 : -1;
}

int ringq_full(ringq *rq) {
  return (rq->tail + 1) % rq->size == rq->head ? 0: -1;
}

static void set_nonblock(int fd, int nonblock) {
  int opts, ret;
  do {
    opts = fcntl(fd, F_GETFL);
  } while(opts < 0 && errno == EINTR);

  if (opts < 0) {
    err_exit("fcntl(%d, F_GETFL) failed.", fd);
  }

  if (nonblock) {
    opts = opts | O_NONBLOCK;
  } else {
    opts = opts | ~O_NONBLOCK;
  }

  do {
    ret = fcntl(fd, F_SETFL, opts);
  } while (ret < 0 && errno == EINTR);

  if (ret < 0) {
    err_exit("fcntl(%d, F_SETFL, opts) failed.", fd);
  }
}

ringq* ringq_new(int size, int use_lock) {
  // assert(size >= RQ_SIZE);

  int ret = 0;
  int mem_sz = sizeof(ringq) + size + 1 + sizeof(ring_item);
  void *mem = malloc(mem_sz);
  if (mem == NULL) {
    err_debug("ringq_new malloc memory error: %s", strerror(errno));
    return NULL;
  }
  memset(mem, 0, mem_sz);

  ringq *rq = mem;
  mem += sizeof(ringq);
  memset(rq, 0, sizeof(ringq));

  rq->mem = mem;
  rq->size = size + 1;
  rq->use_lock = use_lock;

  if (use_lock) {
    pthread_mutexattr_init(&rq->attr);
    ret = pthread_mutex_init(&rq->lock, &rq->attr);
    if (ret < 0) {
      free(rq);
      err_debug("ringq_new init mutex lock error: %s", strerror(ret));
      return NULL;
    }
  }
  return rq;
}

void ringq_dump_tailn(ringq *rq, int n) {
  int i;
  printf("|");
  for (i =n; i< rq->size + sizeof(ring_item); i++) {
    printf("%d|", *((char*)rq->mem + i));
  }
  printf("\n");
}

int ringq_pop(ringq *rq, void *item, int item_size) {
  if (ringq_empty(rq) == 0) {
    /* important! avoid thread to get lock again */
    sched_yield();
    err_debug("ringq is empty, cap:%d, head:%d, tail:%d, num:%d",
      rq->size -1, rq->head, rq->tail, rq->num);
    return -1;
  }
  ring_item *unit;
  int sz, m;
  unit = rq->mem + rq->head;

  int lsz = sizeof(unit->length);
  // read size
  int msize = unit->length + lsz;

  if (rq->tail <= rq->head) {
    sz = rq->size - (rq->head - rq->tail);
    if (sz < msize) {
      err_exit("ringq out of size, cap:%d, ava:%d, head:%d, tail:%d, length:%d, msize:%d, item_size:%d, num:%d",
        rq->size -1, sz, rq->head, rq->tail, unit->length, msize, item_size, rq->num);
      return -1;
    }
    m = rq->size - rq->head;
    if (msize < m) {
      memcpy(item, unit->data, item_size);
      rq->head = (rq->head + msize) % rq->size;
    } else {
      //int head = rq->head;
      //printf("pop: m=%d, head=%d tail=%d\n", m, rq->head, rq->tail);
      //ringq_dump_tailn(rq, head);
      if (lsz < m) {
        memcpy(item, rq->mem + rq->head + lsz, m - lsz);
        rq->head = (rq->head + m) % rq->size;

        memcpy(item + m - lsz, rq->mem + rq->head, unit->length - (m - lsz));
        rq->head = (rq->head + unit->length - (m - lsz)) % rq->size;
      } else {
        //no copy length
        rq->head = (rq->head + lsz) % rq->size;

        memcpy(item, rq->mem + rq->head, item_size);
        rq->head = (rq->head + unit->length) % rq->size;
      }
      //ringq_dump_tailn(rq, head);
    }
  } else {
    sz = rq->tail - rq->head;
    if (sz < msize) {
      err_exit("ringq out of size, cap:%d, ava:%d, head:%d, tail:%d, length:%d, msize:%d, item_size:%d, num:%d",
        rq->size -1, sz, rq->head, rq->tail, unit->length, msize, item_size, rq->num);
      return -1;
    }
    memcpy(item, unit->data, item_size);
    rq->head = (rq->head + msize) % rq->size;
  }

  rq->num--;
  return 0;
}

int ringq_push(ringq *rq, void *item, int item_size) {
  /* when ringq was full */
  if (ringq_full(rq) == 0) {
    sched_yield();
    err_debug("ringq is full, cap:%d, head:%d, tail:%d, num:%d", 
      rq->size -1, rq->head, rq->tail, rq->num);
    return -1;
  }

  ring_item *unit;
  int sz, m;

  unit = rq->mem + rq->tail;

  int lsz = sizeof(unit->length);
  // write size
  int msize = lsz + item_size;

  if (rq->head <= rq->tail) {
    sz = rq->size - (rq->tail - rq->head) -1;
    if (sz < msize) {
      err_debug("ringq out of size, cap:%d, ava:%d, head:%d, tail:%d, length:%d, msize:%d, item_size:%d, num:%d",
        rq->size -1, sz, rq->head, rq->tail, unit->length, msize, item_size, rq->num);
      return -1;
    }
    //import!
    unit->length = item_size;

    m = rq->size - rq->tail;
    if (msize < m) {
      memcpy(unit->data, item, item_size);
      rq->tail = (rq->tail + msize) % rq->size;
    } else {
      //int tail = rq->tail;
      //printf("push: m=%d, head=%d tail=%d\n", m, rq->head, rq->tail);
      //ringq_dump_tailn(rq, tail);
      if (lsz < m) {
        memcpy(rq->mem + rq->tail + lsz, item, m - lsz);
        rq->tail = (rq->tail + m) % rq->size;

        memcpy(rq->mem + rq->tail, item + m - lsz, item_size - (m - lsz));
        rq->tail = (rq->tail + item_size - (m - lsz)) % rq->size;
      } else {
        //no copy length
        rq->tail = (rq->tail + lsz) % rq->size;

        memcpy(rq->mem + rq->tail, item, item_size);
        rq->tail = (rq->tail + item_size) % rq->size;
      }
      //ringq_dump_tailn(rq, tail);
    }
  } else {
    sz = rq->head - rq->tail - 1;
    if (sz < msize) {
      err_debug("ringq out of size, cap:%d, ava:%d, head:%d, tail:%d, length:%d, msize:%d, item_size:%d, num:%d",
        rq->size -1, sz, rq->head, rq->tail, unit->length, msize, item_size, rq->num);
      return -1;
    }

    //import!
    unit->length = item_size;

    memcpy(unit->data, item, item_size);
    rq->tail = (rq->tail + msize) % rq->size;
  }

  rq->num++;
  return 0;
}

int ringq_pop_with_lock(ringq *rq, void *item, int item_size) {
  if (!rq->use_lock) {
    return -1;
  }

  pthread_mutex_lock(&rq->lock);
  int ret = ringq_pop(rq, item, item_size);
  pthread_mutex_unlock(&rq->lock);

  return ret;
}

int ringq_push_with_lock(ringq *rq, void *item, int item_size) {
  if (!rq->use_lock) {
    return -1;
  }

  pthread_mutex_lock(&rq->lock);
  int ret = ringq_push(rq, item, item_size);
  pthread_mutex_unlock(&rq->lock);

  return ret;
}

void ringq_destroy(ringq *rq) {
  if (rq->use_lock){
    pthread_mutex_destroy(&rq->lock);
  }
  if (rq) {
    free(rq);
  }
}

void ringq_dump(ringq *rq) {
  printf("ringq info:mem=%p, size=%d, head=%d, tail=%d, num=%d, meminfo=",
    rq->mem, rq->size -1, rq->head, rq->tail, rq->num);
  int i;
  for (i=0; i < rq->size + sizeof(ring_item); i++) {
    if (i == 0) {
      printf("|");
    }
    printf("%d|", *((char*)rq->mem + i));
  }
  printf("\n");
}

int test_ringq(void) {
  char buf[11] = {0}, rcv[11]={0};
  int i, ret;
  ringq *rq = ringq_new(100, 1);
  for (i =0; i< 10; i++) {
    snprintf(buf, sizeof(buf), "###@@@$$$%d", i % 10);
    err_debug("push item %d, size=%d\n", i, sizeof(buf));
    ret = ringq_push(rq, buf, sizeof(buf)-1);
    ringq_dump(rq);
    if (ret < 0) {
      int j;
      pop:
      for(j =0; j < 10; j++) {
        ret = ringq_pop(rq, rcv, sizeof(rcv)-1);
        ringq_dump(rq);
        if (ret > -1) {
          err_debug("pop item %s\n", rcv);
        }
      }
      ret = ringq_push(rq, buf, sizeof(buf)-1);
      ringq_dump(rq);
      if (ret > -1) {
        err_debug("push %d again ok!\n", i);
      } else {
        goto pop;
      }
    }
  }
  ret = ringq_pop(rq, rcv, sizeof(rcv)-1);
  ret = ringq_pop(rq, rcv, sizeof(rcv)-1);
  ret = ringq_pop(rq, rcv, sizeof(rcv)-1);
  ret = ringq_pop(rq, rcv, sizeof(rcv)-1);
  ret = ringq_pop(rq, rcv, sizeof(rcv)-1);
  ringq_destroy(rq);
  return 0;
}

void * test_f(void *arg) {
  ringq *rq = (ringq*) arg;

  int i, ret;
  char buf[] = "hello world!";
  for (i=0; i < 10000; i++)  {
    ret = ringq_push_with_lock(rq, buf, sizeof(buf));
    if (ret < 0) {
      err_debug("sleep 1 second");
      sleep(1);
    } else err_debug("push ok : %d, size=%d, buf=%s", i, sizeof(buf), buf);
  }
  return NULL;
}

int test_ringq_mulit(void) {
  pthread_t tid;
  ringq *rq = ringq_new(1024*5, 1);
  assert(rq != NULL);
  pthread_create(&tid, NULL, test_f, rq);

  char buf[13] = {0};
  while (1) {
    int ret = ringq_pop_with_lock(rq, buf, sizeof(buf));
    if (ret < 0) {
      sleep(1);
    } else {
      if (strcmp(buf, "hello world!") != 0) {
        err_exit("error: buf=%s", buf);
      } else {
        err_debug("pop ok : %s", buf);
      }
    }
  }
  pthread_join(tid, NULL);
  return 0;
}

/**
 * thread 线程池
 */
#ifndef FAA
#define FAA(a, b) __sync_fetch_and_add(a, b)
#endif
#ifndef FAS
#define FAS(a, b) __sync_fetch_and_sub(a, b)
#endif

typedef struct {
  union {
    void *ptr;
    uint32_t u32;
    uint64_t u64;
  } data;
  int id;
  pthread_t tid;
  ringq *rq;
  int efd;
  int status;
  void (*handler)(void*);
} wunit;

static int wunit_init(wunit *u, int qsz, int id) {
  u->id = id;
  u->efd = eventfd(0, EFD_CLOEXEC);
  if (u->efd < 0) {
    err_debug("create eventfd error:%s", strerror(errno));
    return -1;
  }
  u->rq = ringq_new(qsz, 1);
  if (u->rq == NULL) {
    err_debug("new ringq error");
    return -1;
  }
  return 0;
}

typedef struct {
  wunit *units;
  int num;
  int status;
} wpool;

wpool* wpool_new(int num, int rq_sz) {
  int msz = sizeof(wpool) + num * sizeof(wunit);
  void *mem = malloc(msz);
  if (!mem) {
    err_debug("wpool allocate memory error: %s", strerror(errno));
    return NULL;
  }
  memset(mem, 0, msz);
  wpool *wp = mem;
  mem += sizeof(wpool);

  wp->num = num;
  wp->units = mem;
  int i;
  for (i = 0; i < num; i++) {
    if (wunit_init(&wp->units[i], rq_sz, i+1) < 0) {
      return NULL;
    }
  }
  return wp;
}

static void* wpool_exec(void *arg) {
  wunit *unit = (wunit*)arg;
  for(;;) {
    FAA(&unit->status, 0);
    if (!unit->status) {
      break;
    }
    if (unit->handler != NULL) {
      unit->handler(unit);
    } else {
      sleep(1);
      err_debug("thread [%d] working...", pthread_self());
    }
  }
  err_debug("thread [%d] done!", pthread_self());
  pthread_exit(NULL);
}

void wpool_start(wpool *wp) {
  int i, ret;
  for (i = 0; i < wp->num; i++) {
    wp->units[i].status = 1;
    ret = pthread_create(&wp->units[i].tid, NULL, wpool_exec, &wp->units[i]);
    if (ret< 0) {
      err_exit("wpool_start: pthread created error:%s", strerror(ret));
      return;
    }
  }
  wp->status =1;
}

void wpool_stop(wpool *wp) {
  if (wp->status == 0) {
    return;
  }
  int i, ret;
  uint64_t u;
  size_t s;
  for (i=0; i < wp->num; i++) {
    if (!wp->units[i].tid) {
      err_debug("thread not create, index:%d", i);
      return;
    }
    FAA(&wp->units[i].status, 0);
    while(wp->units[i].status) {
      FAS(&wp->units[i].status, 1);
      u = 0x0F;
      //send signal to wakeup thread when blocking in efd
      s = write(wp->units[i].efd, &u, sizeof(u));
      if (s != sizeof(u)) {
        err_debug("wpool stop error: write efd fail.");
      }
    }
    ret = pthread_join(wp->units[i].tid, NULL);
    if (ret < 0) {
      err_debug("pthread join error: %s", strerror(ret));
    } else {
      err_debug("thread [%d] join ok!", wp->units[i].tid);
    }
  }
  wp->status =0;
}

void wpool_destory(wpool *wp) {
  int i;
  for (i = 0; i < wp->num; i++) {
    ringq_destroy(wp->units[i].rq);
    close (wp->units[i].efd);
  }
  if (wp) {
    free(wp);
  }
}

/**
 * memory pool 内存池
 */
typedef struct _mslab {
  uint8_t used;
  struct _mslab *prev;
  struct _mslab *next;
  char data[0];
} mslab;

typedef struct {
  void *mem;
  size_t size;
  mslab *head;
  mslab *tail;
  uint32_t slab_num;
  uint32_t slab_size;
  uint32_t slab_used;
  pthread_mutex_t lock;
  pthread_mutexattr_t attr;
} mpool;

mpool* mpool_new(uint32_t slab_num, uint32_t slab_size) {
  size_t size = slab_num * (sizeof(mslab) + slab_size);
  size_t lsz = size + sizeof(mpool);

  void *mem = malloc(lsz);
  if (mem == NULL) {
    err_debug("mpool allocate memory error: %s", strerror(errno));
    return NULL;
  }
  memset(mem, 0, lsz);

  mpool *mp =mem;
  mem += sizeof(mpool);
  mp->mem = mem;
  mp->size = size;
  mp->slab_num = slab_num;
  mp->slab_size = slab_size;

  pthread_mutexattr_init(&mp->attr);
  int ret = pthread_mutex_init(&mp->lock, &mp->attr);
  if (ret < 0) {
    err_debug("mpool init mutex error: %s", strerror(ret));
    return NULL;
  }

  //init
  void *cur = mp->mem;
  mslab *slab;
  do {
    slab = (mslab*)cur;
    //head insert
    if (mp->head != NULL) {
      mp->head->prev = slab;
      slab->next = mp->head;
    } else {
      mp->tail = slab;
    }
    mp->head = slab;
    cur += sizeof(mslab) + mp->slab_size;
    if (cur < mp->mem + mp->size) {
      slab->prev = (mslab*)cur;
    } else {
      slab->prev = NULL;
      break;
    }
  } while(1);
  return mp;
}

void* mpool_get(mpool *mp) {
  mslab *s = mp->head;
  if (s->used == 0) {
    //当小于一定的阈值时进行自动扩容

    s->used = 1;
    mp->slab_used++;
    //move next slice to head (idle list)
    mp->head = s->next;
    s->next->prev = NULL;

    //move this slice to tail (busy list)
    mp->tail->next = s;
    s->next = NULL;
    s->prev = mp->tail;
    mp->tail = s;

    return s->data;
  }
  return NULL;
}

void* mpool_get_with_lock(mpool *mp) {
  pthread_mutex_lock(&mp->lock);
  void *data = mpool_get(mp);
  pthread_mutex_unlock(&mp->lock);
  return data;
}

void mpool_put(mpool *mp, void* p) {
  assert(p > mp->mem && p < mp->mem + mp->size);
  mslab *s;
  s = p - sizeof(mslab);
  if (s->used) {
    mp->slab_used--;
  } else {
    return;
  }
  s->used = 0;
  if (s->prev == NULL) {
    return;
  }
  if (s->next == NULL) {
    s->prev->next = NULL;
    mp->tail = s->prev;
  } else {
    s->prev->next = s->next;
    s->next->prev = s->prev;
  }
  s->prev=NULL;
  s->next = mp->head;
  mp->head->prev = s;
  mp->head =s;
}

void mpool_put_with_lock(mpool *mp, void *p) {
  pthread_mutex_lock(&mp->lock);
  mpool_put(mp, p);
  pthread_mutex_unlock(&mp->lock);
}

void mpool_destroy(mpool *mp) {
  pthread_mutex_destroy(&mp->lock);
  if (mp) {
    free(mp);
  }
}

void mpool_dump(mpool *mp) {
  mslab *s = mp->head;
  while (s!=NULL) {
    printf("head:%p, tail:%p, slab:%p, prev:%p, next:%p, used:%d, data:%p\n",
      mp->head, mp->tail, s, s->prev, s->next, s->used, s->data);
    s = s->next;
  }
}

/**
 * event 事件
 */
typedef enum {
  E_READ = 0x01, 
  E_WRITE = 0x02,
} e_kide;

typedef void event_callback(int , void*);

struct event {
  int fd;
  e_kide kide;
  int queued;
  event_callback *cb;
  void *arg;
  TAILQ_ENTRY (event) event_entry;
};

struct ebot_base {
  e_kide kides;
  struct epoll_event *epevents;
  int epfd;
  int epsz;
  uint32_t event_num;
  uint32_t wait_sz;
  struct event **read_waits; //读标记
  struct event **write_waits; //写标记
  TAILQ_HEAD(, event) event_readies; //事件就绪队列
};

// event_read(e, 1, NULL, NULL);
void event_read(struct event *e, int fd, event_callback *cb, void *arg) {
  memset(e, 0, sizeof(*e));
  e->kide |= E_READ;
  e->cb = cb;
  e->arg = arg;
  e->fd = fd;
}

void event_write(struct event *e, int fd, event_callback *cb, void *arg) {
  memset(e, 0, sizeof(*e));
  e->kide |= E_WRITE;
  e->cb = cb;
  e->arg = arg;
  e->fd = fd;
}

static int _resize_epoll_events(struct ebot_base *eb) {
  int newsz = eb->wait_sz << 1;
  int esz = sizeof(struct event*); 
  struct event **r, **w;
  r = realloc(eb->read_waits, newsz * esz);
  if (r == NULL) {
    return -1;
  }
  w = realloc(eb->write_waits, newsz * esz);
  if (w == NULL) {
    free(r);
    return -1;
  }
  eb->read_waits = r;
  eb->write_waits = w;

  memset(r + eb->epsz, 0, (newsz - eb->epsz) * esz);
  memset(w + eb->epsz, 0, (newsz - eb->epsz) * esz);
  eb->wait_sz = newsz;
  return 0;
}

#ifndef EP_SIZE
#define EP_SIZE 1024
#endif

#ifndef FD_CLOSEONEXEC
#define FD_CLOSEONEXEC(x) do { \
  if (fcntl(x, F_SETFD, 1) == -1) { \
    err_exit("fcntl set error: %s", strerror(errno)); \
  } \
} while(0)
#endif


struct ebot_base* ebot_new(e_kide kides) {
  if (((E_READ | E_WRITE) & kides) != kides) {
    err_debug("not supported event kides: %x", kides);
    return NULL;
  }
  struct ebot_base *base = malloc(sizeof(struct ebot_base));
  if (base == NULL) {
    err_debug("ebot base new error: %s", strerror(errno));
    return NULL;
  }
  memset(base, 0, sizeof(struct ebot_base));
  base->kides = kides;
  
  size_t sz = EP_SIZE * sizeof(struct epoll_event);
  base->epevents = malloc(sz);
  assert(base->epevents != NULL);
  memset(base->epevents, 0, sz);


  sz = EP_SIZE * sizeof(struct event*);
  base->read_waits = malloc(sz);
  assert(base->read_waits != NULL);
  memset(base->read_waits, 0, sz);

  base->write_waits = malloc(sz);
  assert(base->write_waits != NULL);
  memset(base->write_waits, 0, sz);

  int epfd = epoll_create(EP_SIZE);
  if (epfd< 0) {
    err_debug("epoll fd create error: %s", strerror(errno));
    return NULL;
  }
  FD_CLOSEONEXEC(epfd);
  base->epfd = epfd;
  base->epsz = EP_SIZE;
  base->wait_sz = EP_SIZE;

  //init queues
  TAILQ_INIT(&base->event_readies);

  return base;
}

int ebot_add_event(struct ebot_base *eb, struct event *e) {
  if (!(e->kide & eb->kides)) {
    errno = ENOTSUP;
    return -1;
  }
  struct event *r = eb->read_waits[e->fd];
  struct event *w = eb->write_waits[e->fd];

  int op = EPOLL_CTL_ADD;
  if (r != NULL || w != NULL) {
    op = EPOLL_CTL_MOD;
  }

  int events = (e->kide & E_READ ? EPOLLIN|EPOLLET : 0) | (e->kide & E_WRITE ? EPOLLOUT : 0);

  if (r != NULL) {
    events |= EPOLLIN;
  }
  if (w != NULL) {
    events |= EPOLLOUT;
  }
  if (e->fd >= eb->wait_sz) {
    // resize
    _resize_epoll_events(eb);
  }

  struct epoll_event epevent;
  epevent.data.u64 = e->fd; 
  epevent.events = events;
  if (epoll_ctl(eb->epfd, op, e->fd, &epevent) < 0) {
    err_debug("epoll_ctl %s error: %s", (op == EPOLL_CTL_ADD ? "EPOLL_CTL_ADD" : "EPOLL_CTL_MOD"), strerror(errno));
    return -1;
  }

  if (e->kide & E_READ && r == NULL) {
    eb->read_waits[e->fd] = e;
  }
  if (e->kide & E_WRITE && w == NULL) {
    eb->write_waits[e->fd] = e;
  }

  err_debug("added io event ok! event:%p, fd:%d, op:%s, events:%s, kide:%s, cp:%p",
    e,
    e->fd,
    op == EPOLL_CTL_ADD ? "EPOLL_CTL_ADD" : "EPOLL_CTL_MOD",
    events ^ (EPOLLIN | EPOLLOUT) ? (events & EPOLLOUT ? "EPOLLOUT": (events & EPOLLIN ? "EPOLLIN": "")) : "EPOLLIN | EPOLLOUT",
    e->kide ^ (E_READ | E_WRITE) ? (e->kide & E_READ ? "E_READ" : (e->kide & E_WRITE ? "E_WRITE": "")) : "E_READ | E_WRITE",
    e->cb
  );
  return 0;
}

int ebot_del_event(struct ebot_base *eb, struct event *e) {
  int op = EPOLL_CTL_DEL;
  int events = (e->kide & E_READ ? EPOLLIN : 0) | (e->kide & E_WRITE ? EPOLLOUT : 0);
  int rd = 1; 
  int wd = 1;

  if (events & (EPOLLIN|EPOLLOUT)) {
    if ((events & EPOLLIN) && eb->write_waits[e->fd] != NULL) {
      wd = 0;
      events = EPOLLOUT;
      op = EPOLL_CTL_MOD;
    } else if ((events & EPOLLOUT) && eb->read_waits[e->fd] != NULL) {
      rd = 0;
      events = EPOLLIN | EPOLLET;
      op = EPOLL_CTL_MOD;
    }
  }

  struct epoll_event epevent;
  epevent.events = events;
  epevent.data.u64 = e->fd;

  if (epoll_ctl(eb->epfd, op, e->fd, &epevent) < 0) {
    err_debug("epoll_ctl %s error: %s", (op == EPOLL_CTL_DEL? "EPOLL_CTL_DEL" : "EPOLL_CTL_MOD"), strerror(errno));
    return -1;
  }

  err_debug ("deleted io event ok! event:%p, fd:%d, op:%s, events:%s, kide:%s, cp:%p",
    e,
    e->fd,
    op == EPOLL_CTL_DEL ? "EPOLL_CTL_DEL": "EPOLL_CTL_MOD",
    events ^ (EPOLLIN | EPOLLOUT) ?  (events & EPOLLOUT ? "EPOLLOUT" : (events & EPOLLIN ? "EPOLLIN": "")) : "EPOLLIN|EPOLLOUT",
    e->kide ^ (E_READ | E_WRITE) ? (e->kide & E_READ ? "E_READ" : ((e->kide & E_WRITE) ? "E_WRITE": "")) : "E_READ | E_WRITE",
    e->cb
  );
  if (rd) {
    eb->read_waits[e->fd] = NULL;
  }
  if (wd) {
    eb->write_waits[e->fd] = NULL;
  }
  return 0;
}

static int queued_event_readies(struct ebot_base *eb, struct event *e) {
  if (!e || e->queued == 1) {
    return -1;
  }
  TAILQ_INSERT_TAIL(&eb->event_readies, e, event_entry);
  e->queued = 1;
  return 0;
}

static void dispatch_events(struct ebot_base* eb) {
  if (TAILQ_EMPTY(&eb->event_readies)) {
    return;
  }
  struct event *e;
  for (e = TAILQ_FIRST(&eb->event_readies); e; e = TAILQ_FIRST(&eb->event_readies)){
    //从队列移除
    TAILQ_REMOVE(&eb->event_readies, e, event_entry);
    e->queued = 0;
    if (e->kide & (E_READ | E_WRITE) && e->cb != NULL) {
      e->cb(e->fd, e->arg);
    }
  }
}

int ebot_loop(struct ebot_base *eb, struct timeval *timeout) {
  int t = 0;
  if (timeout != NULL) {
    t = timeout->tv_sec * 1000 + (timeout->tv_usec + 999) / 1000;
  }

  int c = epoll_wait(eb->epfd, eb->epevents, eb->epsz, t);
  if (c < 0) {
    return errno == EINTR ? 0 : -1;
  }
  int i, fd, kides;
  for (i = 0; i < c; i++) {
    fd = (uint32_t) eb->epevents[i].data.u64;
    kides = (eb->epevents[i].events & EPOLLOUT ? E_WRITE : 0) | (eb->epevents[i].events & EPOLLIN ? E_READ : 0);

    if (kides & E_READ) {
      queued_event_readies(eb, eb->read_waits[fd]);
      err_debug("got event E_READ, dispatched fd:%d event[%p] in event readies queue", fd, eb->read_waits[fd]);
    }

    if (kides & E_WRITE) {
      queued_event_readies(eb, eb->write_waits[fd]);
      //err_debug("got event E_WRITE, dispatched fd:%d event[%p] in event readies queue", fd, eb->write_waits[fd]);
    }
  }
  dispatch_events(eb);
  return 0;
}

void ebot_destroy(struct ebot_base *eb) {
  close(eb->epfd);
  free(eb->epevents);
  free(eb->write_waits);
  free(eb->read_waits);
  free(eb);
}

void ebot_dump(struct ebot_base *eb) {
  printf("eb:%p, kides: 0x%x, event_num:%d, epoll_fd:%d, epoll_size:%d, epoll_event:%p \n",
    eb, eb->kides, eb->event_num, eb->epfd, eb->epsz, eb->epevents);

  printf("read waits:\n");
  int i;
  struct event *s;
  for (i=0; i < eb->wait_sz; i++) {
    s = eb->read_waits[i];
    if (s != NULL && s->kide & E_READ) {
      printf("\t{");
      printf("%p, kide: %s, fd: %d, cb: %p, arg: %p", s, s->kide & E_READ ? "E_READ": "''", s->fd, s->cb, s->arg);
      printf("}\n");
    }
  }
  printf("write waits:\n");
  for (i=0; i < eb->wait_sz; i++) {
    s = eb->write_waits[i];
    if (s != NULL && s->kide & E_WRITE) {
      printf("\t{");
      printf("%p, kide: %s, fd: %d, cb: %p, arg: %p", s, s->kide & E_WRITE? "E_WRITE": "''", s->fd, s->cb, s->arg);
      printf("}\n");
    }
  }
  printf("event readies:\n");
  TAILQ_FOREACH(s, &eb->event_readies, event_entry) {
    printf("\t{");
    printf("%p, kide: %s, fd: %d, cb: %p, arg: %p", s, s->kide & E_WRITE? "E_WRITE": (s->kide & E_READ ? "E_READ": ""), 
      s->fd, s->cb, s->arg);
    printf("}\n");
  }
}

/**
 * reactor 反应堆
 */
typedef struct {
  int id;
  pthread_t tid;
  struct ebot_base *base;
  void *srv;
  int pipe[2];
  int status;
} reactor; 

typedef struct {
  int fd;
  int len;
  char buf[STREAM_BUF_SIZE];
} pack_data;

typedef struct {
  reactor *reactors;
  int rsz;
} reactor_pool;

reactor_pool* reactor_pool_new(int rsz) {
  int sz = sizeof (reactor_pool) + sizeof(reactor) *rsz;
  void *mem = malloc(sz);
  if (mem == NULL) {
    err_exit("malloc reactor_pool error: %s", strerror(errno));
  }
  memset(mem, 0, sz);
  reactor_pool *rp = mem;
  mem += sizeof(reactor_pool);
  rp->rsz = rsz; 
  rp->reactors = mem;
  int i;
  struct ebot_base *base;
  for (i=0; i < rsz; i++) {
    rp->reactors[i].id = i+1;
    base = ebot_new(E_READ | E_WRITE);
    assert(base != NULL);
    rp->reactors[i].base = base;

    if (pipe(rp->reactors[i].pipe) < 0) {
      err_exit("reactor pool create pipe error: %s", strerror(errno));
    }
    set_nonblock(rp->reactors[i].pipe[0], 1);
    set_nonblock(rp->reactors[i].pipe[1], 1);
  }
  return rp;
}

static int readn(int fd, char *buf, int len) {
  int n = 0, nread;
  while (1) {
    nread = read(fd, buf + n, len -n);
    if (nread < 0) {
      if (errno == EINTR){
        continue;
      }
      break;
    } else if (nread == 0) {
      return 0;
    } else {
      n += nread;
      if (n == len) {
        break;
      }
      continue;
    }
  }
  return n;
}

static int writen(int fd, char *buf, int len) {
  int n = 0;
  int total = 0;
  while (total != len) {
    n = write(fd, buf, len - total);
    if (n == -1) {
      return total;
    }
    if (n == -1) {
      if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN) {
        sleep(1);
        continue;
      } else {
        return -1;
      }
    }
    total += n;
    buf += n;
  }
  return total;
}

static wpool* _get_eb_srv_factory(void *srv);
static mpool* _get_eb_srv_event_pool(void *srv);
static void (* _get_eb_srv_callback(void *srv, eb_event)) (eb_stream*);
static void* _get_eb_srv_udata_ptr(void *srv);

static void reactor_pool_client_read_callback(int fd, void *arg) {
  reactor *r = (reactor *) arg;
  pack_data data = {fd, 0, {0} };
  int id = fd % _get_eb_srv_factory(r->srv)->num;

  int n =readn(fd, data.buf, sizeof(data.buf));
  if (n == 0) {
    if (r->base->read_waits[fd] != NULL) {
      ebot_del_event(r->base, r->base->read_waits[fd]);
    }
    if (r->base->write_waits[fd] != NULL) {
      ebot_del_event(r->base, r->base->write_waits[fd]);
    }
    err_debug("remote client fd=%d close", fd);
  } else if (n > 0) {
    data.fd = fd;
    data.len = n;
  } else {
    //TODO
    err_debug("read fd buf less then 0!!!!!!!!!");
    return;
  }
  if (ringq_push_with_lock(_get_eb_srv_factory(r->srv)->units[id].rq, &data, sizeof(data)) < 0) {
    err_debug("ringq push client data buf fail: fd=%d, reactor_id=%d, factory_unit_id=%d", fd, r->id, _get_eb_srv_factory(r->srv)->units[id].id);
    return;
  }
  //通知工作线程
  uint64_t sig = 0x01;
  if (write(_get_eb_srv_factory(r->srv)->units[id].efd, &sig, sizeof(sig)) < 0) {
    err_debug("notify factory unit fail: reactor_id=%d, factory_unit_id=%d", r->id, _get_eb_srv_factory(r->srv)->units[id].id);
  }
}

static void reacto_pool_client_write_callback(int fd, void *arg) {
  reactor *r = (reactor*) arg;
  //TODO
  if ( _get_eb_srv_callback(r->srv, EB_WRITE) != NULL) {
    eb_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.fd = fd;
    stream.udata.ptr = _get_eb_srv_udata_ptr(r->srv);
    _get_eb_srv_callback(r->srv, EB_WRITE)(&stream);
  }
}

static void reactor_pool_pipe_callback(int pfd, void *arg) {
  reactor *r = (reactor*) arg;
  int fd;
  if (read(r->pipe[0],&fd, sizeof(fd)) < 0) {
    err_debug("read pipe fd error: %s", strerror(errno));
    return;
  }
  err_debug("recv client fd=%d from server", fd);

  struct event *e = mpool_get_with_lock(_get_eb_srv_event_pool(r->srv));
  if (e == NULL) {
    return;
  }
  event_read(e, fd, reactor_pool_client_read_callback, r);
  if (ebot_add_event(r->base, e) < 0) {
    err_debug("ebot base add client fd[%d] read event fail", fd);
  }

  e = mpool_get_with_lock(_get_eb_srv_event_pool(r->srv));
  if (e == NULL) {
    return;
  }
  event_write(e, fd, reacto_pool_client_write_callback, r);
  if (ebot_add_event(r->base, e) < 0) {
    err_debug("ebot base add client fd[%d] write event fail", fd);
  }
}

void* reactor_pool_exec(void *arg) {
  reactor *r = (reactor*) arg;
  struct timeval tv = {1, 0};
  
  struct event *e = mpool_get_with_lock(_get_eb_srv_event_pool(r->srv));
  assert(e != NULL);
  event_read(e, r->pipe[0], reactor_pool_pipe_callback, r);
  if (ebot_add_event(r->base, e) < 0) {
    err_exit("ebot base add pipe fd[%d] read event fail", r->pipe[0]);
  }

  while(r->status) {
    ebot_loop(r->base, &tv);
  }
  return NULL;
}

void reactor_pool_start(reactor_pool *rp, void* (*exec)(void*)) {
  int i, ret;
  for (i = 0; i < rp->rsz; i++) {
    rp->reactors[i].status = 1;
    ret = pthread_create(&rp->reactors[i].tid, NULL, exec, &rp->reactors[i]);
    if (ret < 0) {
      err_debug("create reactor thread error: %s", strerror(ret));
      exit(-1);
    }
  }
}

void reactor_pool_stop(reactor_pool *rp) {
  int i, ret;
  for (i = 0; i<rp->rsz; i++) {
    rp->reactors[i].status = 0;
    close(rp->reactors[i].pipe[0]);
    close(rp->reactors[i].pipe[1]);
    ret = pthread_join(rp->reactors[i].tid, NULL);
    if (ret < 0) {
      err_debug("join reacto thread error: %s", strerror(ret));
      exit(-1);
    }
  }
}

void reactor_pool_destroy(reactor_pool *rp) {
  int i;
  for (i=0; i < rp->rsz; i++) {
    ebot_destroy(rp->reactors[i].base);
  }
  free(rp);
}

/*
 * server 实例
 */

typedef struct {
  socklen_t len;
  union {
    struct sockaddr sa;
    struct sockaddr_in sin;
  } u;
#ifdef WITH_IPV6
  struct sockaddr_in6 sin6;
#endif
} sa;

struct ebot_srv {
  eb_settings *setting;
  union {
    void *ptr;
    uint32_t u32;
    uint64_t u64;
  } udata;
  mpool *event_pool;
  struct ebot_base *master_base;
  reactor_pool *slave_reactor;
  wpool *factory;
  wpool *writer;
  int total_conns;
  int sfd;
  int status;
  void (*callbacks[8])(eb_stream *);
};

static wpool* _get_eb_srv_factory(void *srv) {
  return ((ebot_srv*)srv)->factory;
}

static mpool* _get_eb_srv_event_pool(void *srv) {
  return ((ebot_srv*)srv)->event_pool;
}

static void (* _get_eb_srv_callback(void *srv, eb_event e)) (eb_stream*) {
  return ((ebot_srv*)srv)->callbacks[e];
}

static void* _get_eb_srv_udata_ptr(void *srv) {
  return ((ebot_srv*)srv)->udata.ptr;
}

static void ebot_srv_accept(int sfd, void *arg) {
  ebot_srv *srv = (ebot_srv*) arg;
  int fd;
  sa sa;
  sa.len = sizeof(sa.u.sa);
  fd = accept(srv->sfd, &sa.u.sa, &sa.len);
  if (fd < 0) {
    err_debug("server fd=%d accept error: %s", srv->sfd, strerror(errno));
    return;
  } 
  if (srv->total_conns > srv->setting->max_connections) {
    close(fd);
    return;
  }
  set_nonblock(fd, 1);

  //通过管道发送给从反应堆
  int id = fd % srv->slave_reactor->rsz;
  int pfd = srv->slave_reactor->reactors[id].pipe[1];

  if (write(pfd, &fd, sizeof(fd)) < 0) {
    err_debug("write to slave reactor[%d] pipe fd[%d]", id, pfd);
  }
  if (srv->callbacks[EB_CONNECT] != NULL) {
    eb_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.fd = fd;
    stream.udata.ptr = srv->udata.ptr;
    srv->callbacks[EB_CONNECT](&stream);
  }
  srv->total_conns++;
}

static void eb_srv_factory_exec(void* arg) {
  wunit *factory_unit = (wunit*) arg;
  ebot_srv *srv = (ebot_srv*)factory_unit->data.ptr;
  pack_data data;
  
  if (ringq_pop_with_lock(factory_unit->rq, &data, sizeof(data)) > -1) {
    err_debug("facotry unit[id=%d] recv package: pack_size=%d, fd=%d, len=%d, buf=%s", factory_unit->id, sizeof(data), data.fd, data.len, data.buf);
    eb_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.fd = data.fd;
    stream.buf_size = data.len;
    stream.udata.ptr = srv->udata.ptr;
    memcpy(stream.buf, data.buf, data.len);

    if (stream.buf_size == 0) {
      if (srv->callbacks[EB_CLOSE] != NULL) {
        srv->callbacks[EB_CLOSE](&stream);
      }
    } else {
      if (srv->callbacks[EB_RECV] != NULL) {
        srv->callbacks[EB_RECV](&stream);
      }
    }
  } else {
    uint64_t sig;
    int ret = read(factory_unit->efd, &sig, sizeof(sig));
    if (ret < 0) {
      err_debug("read sig error: %s", strerror(errno));
    }
  }
}

//默认srv配置
eb_settings eb_default_setting = {
  .backlog = 0,
  .memory_pool_size = 1024 * 4,
  .reactor_num = 4,
  .factory_num = 4,
  .writer_num = 4,
  .max_connections = 10086,
  .facotry_ringq_size = 1024 * 10,
  .timeout = 0
};

// new server
ebot_srv* eb_srv_new(eb_settings *s) {
  ebot_srv *srv = malloc(sizeof(ebot_srv));
  if (srv == NULL) {
    err_exit("malloc ebot_srv error: %s", strerror(errno));
  }
  memset(srv, 0, sizeof(ebot_srv));

  if (s != NULL) {
    srv->setting = s;
  } else {
    srv->setting = &eb_default_setting;
    s = srv->setting;
  }

  //内存池
  int psz = s->memory_pool_size;
  if (psz <= 0) {
    psz = 4;
  }
  srv->event_pool = mpool_new(psz, sizeof(struct event));
  assert(srv->event_pool != NULL);

  //主反应堆
  srv->master_base = ebot_new(E_READ);
  assert(srv->master_base != NULL);

  //从反应堆线程池
  int rsz = s->reactor_num;
  if (rsz <=0) {
    rsz = 4;
  }
  srv->slave_reactor = reactor_pool_new(rsz);
  assert(srv->slave_reactor != NULL);

  //工作线程池
  int fsz = s->factory_num;
  if (fsz <= 0) {
    fsz = 4;
  }
  srv->factory = wpool_new(fsz, 1024 * 10);
  assert(srv->factory != NULL);

  int i;
  for (i=0; i< fsz; i++) {
    srv->factory->units[i].data.ptr = srv;
    srv->factory->units[i].handler = eb_srv_factory_exec;
  }

  for (i=0; i < rsz; i++) {
    srv->slave_reactor->reactors[i].srv = srv;
  }

  return srv;
}

int eb_srv_listen(ebot_srv *srv, size_t port) {
  int sfd, on =1, af;
  sa sa;

#ifdef WITH_IPV6
  af = PF_INET6;
#else
  af = PF_INET;
#endif

  if (((sfd) = socket(af, SOCK_STREAM, 6)) == -1) {
    err_exit("create socket fail: %s", strerror(errno));
  }

  set_nonblock(sfd, 1);
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on));

#ifdef WITH_IPV6
  sa.u.sin6.sin6_family = af;
  sa.u.sin6.sin6_port = htons(port);
  sa.u.sin6.sin6_addr = in6addr_any;
  sa.len = sizeof(sa.u.sin6);
#else
  sa.u.sin.sin_family = af;
  sa.u.sin.sin_port = htons(port);
  sa.u.sin.sin_addr.s_addr = INADDR_ANY;
  sa.len = sizeof(sa.u.sin);
#endif

  if (bind(sfd, &sa.u.sa, sa.len) < 0) {
    err_exit("bind: af=%d port=%d err=%s", af, port, strerror(errno));
  }

  if (listen(sfd, 16) < 0) {
    err_exit("listen: af=%d port=%d err=%s", af, port, strerror(errno));
  }
  srv->sfd = sfd;
  err_msg("server listening on port:%d", port);
  return 0;
}

int eb_srv_start(ebot_srv *srv) {
  if (srv->sfd <=0) {
    err_exit("server not listen up");
  }

  int ret;
  struct timeval tv={1, 0};

  srv->status = 1;

  //先启动slave reactor
  reactor_pool_start(srv->slave_reactor, reactor_pool_exec);

  //启动工作池
  wpool_start(srv->factory);

  //启动master reactor
  struct event *event;
  event = mpool_get_with_lock(srv->event_pool);
  assert(event != NULL);

  event_read(event, srv->sfd, ebot_srv_accept, srv);
  ret = ebot_add_event(srv->master_base, event);
  if (ret < 0 ) {
    err_exit("server listen fd=%d event add fail", srv->sfd);
  }
  //监听
  while(srv->status) {
    ebot_loop(srv->master_base, &tv);
  }
  return 0;
}

void eb_srv_on(ebot_srv *srv, eb_event e, void (*f)(eb_stream *stream)) {
  if (f == NULL || e > EB_CLOSE) {
    err_msg("callback `f` function is NULL or pararm e=%d is invalid", e);
  } else {
    srv->callbacks[e] = f;
  }
}

void eb_srv_register_ptr(ebot_srv *srv, void *data) {
  srv->udata.ptr = data;
}

void eb_srv_register_u32(ebot_srv *srv, uint32_t n) {
  srv->udata.u32 = n;
}

void eb_srv_register_u64(ebot_srv *srv, uint64_t n) {
  srv->udata.u64 = n;
}

void eb_srv_destroy(ebot_srv *srv) {
  srv->status = 0;

  reactor_pool_stop(srv->slave_reactor);
  wpool_stop(srv->factory);

  reactor_pool_destroy(srv->slave_reactor);
  wpool_destory(srv->factory);
  mpool_destroy(srv->event_pool);
  close(srv->sfd);
  ebot_destroy(srv->master_base);

  free(srv);
}
