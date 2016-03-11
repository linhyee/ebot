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
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/******************************************************************/
/* error functions                                                */
/******************************************************************/
#define ERR_MAXLINE 2048

static void err_doit(int, const char *, va_list);

static void err_sys_(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    err_doit(1, fmt, ap);
    va_end(ap);

    exit(1);
}

static void err_msg_(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    err_doit(0, fmt, ap);
    va_end(ap);

    return;
}

/**
  * print message and return to caller
  * 
  */
static void err_doit(int errnoflag, const char *fmt, va_list ap)
{
    int errno_save, n;
    char buf[ERR_MAXLINE + 1];
    errno_save = errno;

#ifdef HAVE_VSNPRINTF
    vsnprintf(buf, ERR_MAXLINE, fmt, ap);
#else
    vsprintf(buf, fmt, ap);
#endif

    n = strlen(buf);
    if (errnoflag)
        snprintf(buf + n, ERR_MAXLINE - n, ": %s", strerror(errno_save));

    strcat(buf, "\n");
    fflush(stdout);
    fputs(buf, stderr);
    fflush(stderr);

    return;
}
#define __QUOTE(x) # x
#define  _QUOTE(x) __QUOTE(x)

#define err_msg(fmt, ...) do {                                       \
    time_t t = time(NULL);                                           \
    struct tm *dm = localtime(&t);                                   \
    err_msg_("[%02d:%02d:%02d] %s:[" _QUOTE(__LINE__) "]\t    %-26s:"\
        fmt,                                                         \
        dm->tm_hour,                                                 \
        dm->tm_min,                                                  \
        dm->tm_sec,                                                  \
        __FILE__,                                                    \
        __func__,                                                    \
        ## __VA_ARGS__);                                             \
} while(0) 

#define err_print(fmt, func, ...) do { \
    /* TODO:*/                         \
} while(0)

#ifdef DEBUG
#define err_debug(fmt, ...) err_msg(fmt, ## __VA_ARGS__)
#else
#define err_debug(fmt, ...)
#endif

/******************************************************************/
/* cqueue                                                         */
/******************************************************************/
enum q_flag
{
    QF_LOCK   = 1u << 1,
    QF_NOTIFY = 1u << 2,
    QF_SHM    = 1u << 3,
};

struct cq_item
{
    int length;
    char data[0];
};

struct cqueue 
{
    int head;                   /* queue head */
    int tail;                   /* queue tail */
    int capacity;               /* the queue capacity */
    char head_tag;              /* tag whether elem already in head */
    char tail_tag;              /* tag whether elem already in tail*/
    int num;                    /* current total elements */
    int flags;                  /* queue flags supported */
    int max_elemsize;           /* max element size */
    void *mem;                  /* memory block */
    pthread_mutex_t lock;
    pthread_mutexattr_t attr;
    int pipes[2];
};

#define CQ_MINMEMORY_CAPACITY       (1024 * 64) //最小内存分配
#define cqueue_empty(q)             (q->num == 0)
#define cqueue_full(q)              ((q->head == q->tail) && ( q->tail_tag != q->head_tag))

static set_nonblock(int fd, int nonblock)
{
    int opts, ret;

    do {
        opts = fcntl(fd, F_GETFL);
    }
    while(opts < 0 && errno == EINTR);

    if (opts < 0)
    {
        err_msg("fcntl(%d, F_GETFL) failed.", fd);
        exit(1);
    }

    if (nonblock)
        opts = opts | O_NONBLOCK;
    else
        opts = opts | ~O_NONBLOCK;

    do
    {
        ret = fcntl(fd, F_SETFL, opts);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
        err_msg("fcntl(%d, F_SETFL, opts) failed.", fd);
}

struct cqueue * cqueue_new(int capacity, int max_elemsize, enum q_flag flags)
{
    assert(capacity > CQ_MINMEMORY_CAPACITY + max_elemsize);

    void *mem;
    int  ret = 0;

    /* use share memory */
    if (flags & QF_SHM)
    {
        int  shmfd    = -1;
        int  shmflag  = MAP_SHARED;
        char *mapfile = "/dev/zero";

#ifdef MAP_ANONYMOUS
        shmflag |= MAP_ANONYMOUS;
#else
        if ((shmfd = open(mapfile, O_RDWR)) < 0)
            return NULL;        
#endif

        mem = mmap(NULL, capacity, PROT_READ | PROT_WRITE, shmflag, shmfd, 0);

#ifdef MAP_FAILED
        if (mem == MAP_FAILED)
#else
        if (!mem)
#endif
        {
            err_msg("mmap failed, error for %s", strerror(errno));
            return NULL;
        }
    }
    else
    {
        mem = malloc(capacity);

        if (mem == NULL)
        {
            err_msg ("malloc fail!");
            return NULL;
        }
    }

    struct cqueue *cq = mem;
    mem += sizeof(struct cqueue);
    memset(cq, 0, sizeof(struct cqueue));

    cq->mem          = mem;
    cq->capacity     = capacity;
    cq->max_elemsize = max_elemsize;
    cq->flags        = flags;

    if (flags & QF_LOCK)
    {
        pthread_mutexattr_init(&cq->attr);
        pthread_mutexattr_setpshared(&cq->attr, PTHREAD_PROCESS_SHARED);

        ret = pthread_mutex_init(&cq->lock, &cq->attr);

        if (ret < 0)
        {
            if (flags & QF_SHM)
                munmap(mem, capacity);
            else
                free(mem);

            err_msg ("mutext init failed!");

            return NULL;
        }
    }

    if (flags & QF_NOTIFY)
    {
        ret = pipe(cq->pipes);
        if (ret < 0)
        {
            err_msg("pipe create fail. error: %s[%d]", strerror(errno), errno);
        }
        else
        {
            set_nonblock(cq->pipes[0], 1);
            set_nonblock(cq->pipes[1], 1);
        }
    }

    return cq;
}

int cqueue_shift(struct cqueue *cq, void *item, int item_size)
{
    /* if cqueue was empty*/
    if (cqueue_empty(cq))   
    {
        // err_msg("cqueue is empty.");
        /* important! avoid thread to get lock again */
        sched_yield();
        //usleep(1);
        return -1;
    }

    struct cq_item *unit = cq->mem + cq->head;
    assert(item_size >= unit->length);

    memcpy(item, unit->data, unit->length);
    cq->head += (unit->length + sizeof(unit->length));

    if (cq->head >= cq->capacity)
    {
        cq->head = 0;
        cq->head_tag = 1 - cq->head_tag;
    }

    cq->num--;

    return unit->length;
}

int cqueue_unshift(struct cqueue *cq, void *item, int item_size)
{
    assert(item_size < cq->max_elemsize);

    /* when cqueue was empty! */
    if (cqueue_full(cq))
    {
        // err_msg("cqueue is full.");
        sched_yield();

        return -1;
    }

    struct cq_item *unit;
    int msize;

    msize = sizeof(unit->length) + item_size;

    if (cq->tail < cq->head)
    {
        if ((cq->head - cq->tail) < msize)
            return -1;

        unit = cq->mem + cq->tail;
        cq->tail += msize;
    }
    else
    {
        unit = cq->mem + cq->tail;
        cq->tail += msize;
        if (cq->tail >= cq->capacity)
        {
            cq->tail = 0;
            cq->tail_tag = 1 - cq->tail_tag;
        }
    }
    cq->num++;
    unit->length = item_size;
    memcpy(unit->data, item, item_size);

    return 0;
}


int cqueue_pop(struct cqueue *cq, void *item, int item_size)
{
    assert(cq->flags & QF_LOCK);

    int ret = 0;

    pthread_mutex_lock(&cq->lock);
    ret = cqueue_shift(cq, item, item_size);
    pthread_mutex_unlock(&cq->lock);

    return ret;
}

int cqueue_push(struct cqueue *cq, void *item, int item_size)
{
    assert(cq->flags & QF_LOCK);

    int ret = 0;

    pthread_mutex_lock(&cq->lock);
    ret = cqueue_unshift(cq, item, item_size);
    pthread_mutex_unlock(&cq->lock);

    return ret;
}

int cqueue_wait(struct cqueue *cq)
{
    uint64_t data;
    return read(cq->pipes[0], &data, sizeof(data));
}

int cqueue_notify(struct cqueue *cq)
{
    uint64_t data = 1;
    return write(cq->pipes[1], &data, sizeof(data));
}

void cqueue_free(struct cqueue *cq)
{
    if (cq->flags & QF_LOCK)
        pthread_mutex_destroy(&cq->lock);

    if (cq->flags & QF_NOTIFY)
    {
        close(cq->pipes[0]);
        close(cq->pipes[1]);
    }

    if (cq->flags & QF_SHM)
        munmap(cq, cq->capacity);
    else
        free(cq);
}

void printCqueue(struct cqueue *cq)
{
    err_msg("cq: [adr=%p] [mem=%p] [num=%d] [head=%d] [tail=%d] [tail_tag=%d] [head_tag=%d]", cq, cq->mem, cq->num, cq->head, cq->tail, (int)cq->tail_tag, (int)cq->head_tag);
}

/******************************************************************/
/* thread pool                                                    */
/******************************************************************/
#define atom_add(a, b) __sync_fetch_and_add(a, b)
#define atom_sub(a, b) __sync_fetch_and_sub(a, b)
struct thread_pool;
typedef volatile uint32_t _u32_t;

enum thread_stats
{
    T_IDEL,
    T_WAITING,
    T_RUNNIG,
};

struct thread_param
{
    void *data;
    int id;
};

struct thread_entity
{
    union {
        void *ptr;
        uint32_t u32;   
        uint64_t u64;
    } data;

    int         id;
    pthread_t   thread_id;
    enum        thread_stats stats;

    struct      cqueue *cq;
    int         notify_send_fd;
    int         notify_recv_fd;
    struct      thread_pool *pool;
};

struct thread_pool
{
    pthread_mutex_t         mutex;
    pthread_cond_t          cond;
    struct thread_entity    *threads;
    struct thread_param     *params;
    struct cqueue           *cq;
    int                     num_threads;
    int                     shutdown;
    _u32_t                  num_tasks;
};

static void thread_setup(struct thread_entity *me)
{
    me->cq = cqueue_new(1024 * 256, 512, 0);
    if (me->cq == NULL)
    {
        err_msg("can't allocate memory for cq queue!");
        exit(EXIT_FAILURE);
    }
}

static void thread_cleanup(struct thread_entity *me)
{
    cqueue_free(me->cq);    
    close(me->notify_send_fd);
    close(me->notify_recv_fd);
}

/**
 * 初始化线程池
 */
int thread_pool_init(struct thread_pool *pool, int num_threads)
{
    assert(num_threads > 0);
    int i;
    memset(pool, 0, sizeof(struct thread_pool));

    pool->threads = calloc(num_threads, sizeof(struct thread_entity));
    if (!pool->threads)
    {
        err_msg("can't allocate thread descriptors!");
        exit(1);
    }

    pool->params  = calloc(num_threads, sizeof(struct thread_param));
    if (pool->params == NULL)
    {
        err_msg("can't allocate thread params!");
        exit(1);
    }

    for (i = 0; i <num_threads; i++)
    {
        int fds[2];
        if (pipe(fds))
        {
            err_msg("can't create notify pipe!");
            exit(1);
        }

        pool->threads[i].id = i;
        pool->threads[i].notify_recv_fd = fds[0];
        pool->threads[i].notify_send_fd = fds[1];

        thread_setup(&pool->threads[i]);
    }

    pool->cq = cqueue_new(1024 * 256, 512, 0);

    if (pool->cq == NULL)
    {
        err_msg("can't create cq queue!");
        exit(1);
    }

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    pool->num_threads = num_threads;
    pool->shutdown = 1;

    return 0;
}

void thread_pool_run(struct thread_pool *pool, void *(*func)(void *))
{
    pthread_attr_t attr;
    int i, ret;

    pthread_attr_init(&attr);

    for (i = 0; i < pool->num_threads; i++)
    {
        pool->params[i].id = i;
        pool->params[i].data = pool;
        ret = pthread_create(&((pool->threads[i]).thread_id), &attr, func, &pool->params[i]);

        if (ret < 0)
        {
            err_msg("can't create thread, error for %s!", strerror(errno));
            exit(1);
        }
    }

    pool->shutdown = 0;
}

int thread_pool_free(struct thread_pool *pool)
{
    if (pool->shutdown)
        return -1;

    pthread_cond_broadcast(&pool->cond);

    int i;
    for (i = 0; i < pool->num_threads; i++)
    {
        pthread_join((pool->threads[i]).thread_id, NULL);
        thread_cleanup(&pool->threads[i]);
    }

    cqueue_free(pool->cq);
    free(pool->threads);
    free(pool->params);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);

    return 0;
}

/**
 * 线程以竞争的方式抢占任务
 */
int thread_pool_dispatchq(struct thread_pool *pool, void *task, int task_len)
{
    int i, ret;
    pthread_mutex_lock(&pool->mutex);

    //尝试1000次, 将任务打进队列
    for (i = 0; i < 1000; i++)
    {
        ret = cqueue_unshift(pool->cq, task, task_len);

        if (ret < 0)
        {
            usleep(i);
            continue;
        }
        else break;
    }

    pthread_mutex_unlock(&pool->mutex);

    if (ret < 0)
        return -1;

    _u32_t *num_tasks = &pool->num_tasks;
    atom_add(num_tasks, 1);

    return pthread_cond_signal(&pool->cond);
}

/******************************************************************/
/* Reactor                                                        */
/******************************************************************/
typedef void e_cb_t(short events, void *arg);
struct eb_t;
struct eb_o;

enum e_opt
{
    E_ONCE = 0x01,  /* 一次性事件, 当事件dipsatch到队列后,  激活后立该从eb_t移除, 并标记为ONCE */
    E_FREE = 0x02   /* 事件已经从eb_t实例被移除, 将其标记为E_FREE, 以便释放其存储空间 */
};

enum
{
    E_QUEUE = 0x80 //标记是否已经在dispatchq队列
};

enum e_kide
{
    E_READ   = 0x01,    /* IO读 */
    E_WRITE  = 0x02,    /* IO写 */
    E_TIMER  = 0x04,    /* 定时器 */
    E_SIGNAL = 0x08,    /* 信号 */
    E_CHILD  = 0x10,    /* 进程 */
    E_FLAG   = 0x20     /* 用户自定义 */
};

struct ev
{
    enum e_kide kide;           /* 事件类型 */
    enum e_opt opt;             /* 事件的标记 */
    e_cb_t *cb;                 /* 事件回调函数 */
    struct eb_t *ebt;           /* 指向eb_t结构体的实例 */
    void *arg;                  /* 事件参数 */

    TAILQ_ENTRY (ev) dispatchq; /* TAILQ_ENTRY */
};

/* for io event */
struct ev_io
{
    struct ev event;
    int fd;
};

/* for timer event */
struct ev_timer
{
    struct ev event;
    struct timeval tv, remain;
    RB_ENTRY (ev_timer) timer_node;
};

/* signal event */
struct ev_signal
{
    struct ev event;
    int signal;
};

/* for process */
struct ev_child
{
    struct ev event;
    pid_t child;
};

/* for user-self event */
struct ev_flag
{
    struct ev event;
    int flag;
    TAILQ_ENTRY (ev_flag) flags;
};

/**
 * 反应堆基类
 */
struct eb_t
{
    const struct eb_o *ebo;         /* 操作eb_t实例的对象 */
    enum e_kide kides;              /* 所允许的支持事件类型 */
    unsigned int num;               /* 注册到eb_t实例的事件总数 */
    unsigned int numtimers;         /* 当前定时器的总数 */
    unsigned int maxtimers;         /* 最大定时器数 */
    struct timeval timerdebt;       /* 用于定时器相减 */
    int broken;                     /* 中断调用 */

    TAILQ_HEAD(, ev) dispatchq;     /* 事件就绪队列 */
    TAILQ_HEAD(, ev_flag) flags;    /* 自定义事件队列 */
    RB_HEAD(timer_tree, ev_timer) timers; /* 定时器队列 */
};

/**
 * ebt oprerations 反应堆的实例操作元
 */
struct eb_o
{
    const char      *name;
    enum e_kide     kides;
    size_t          ebtsz;  /* 从eb_t派生的结构体的大小 */

    int (*construct)(struct eb_t *);
    int (*destruct) (struct eb_t *);
    int (*init)     (struct eb_t *);
    int (*loop)     (struct eb_t *, const struct timeval *);
    int (*attach)   (struct eb_t *, struct ev *);   
    int (*detach)   (struct eb_t *, struct ev *);
    int (*free) (struct eb_t *);
};

static compare(struct ev_timer *a, struct ev_timer *b)
{
    if (timercmp(&a->remain, &b->remain, <))
        return -1;
    else if (timercmp(&a->remain, &b->remain, >))
        return 1;

    if (a < b)
        return -1;
    else if (a > b)
        return 1;

    return 0;
}

RB_PROTOTYPE(timer_tree, ev_timer, timer_node, compare);
RB_GENERATE(timer_tree, ev_timer, timer_node, compare);

/******************************************************************/
/* epoll functions                                                */
/******************************************************************/
#define EP_SIZE 32000

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do {      \
    if (fcntl(x, F_SETFD, 1) == -1) \
        err_sys_("fcntl error!");   \
} while(0)
#else
#define FD_CLOSEONEXEC(x)
#endif

struct ebt_epoll
{
    struct eb_t ebt;

    struct epoll_event *epevents;
    struct ev_io **readev;
    struct ev_io **writev;
    int epfd;
    int epsz;
    int nfds;
};

static int epoll_init       (struct eb_t *);
static int epoll_loop       (struct eb_t *, const struct timeval *);
static int epoll_attach     (struct eb_t *, struct ev *);
static int epoll_detach     (struct eb_t *, struct ev *);
static int epoll_free       (struct eb_t *);

// void eventq_in(struct ev *);
int eventq_in(struct ev *);

const struct eb_o ebo_epoll = {
    .name   = "epoll",
    .kides  = E_READ | E_WRITE | E_TIMER,
    .ebtsz  = sizeof(struct ebt_epoll),
    .init   = epoll_init,
    .loop   = epoll_loop,
    .attach = epoll_attach,
    .detach = epoll_detach,
    .free   = epoll_free
};

/**
 * epoll初始化
 * 
 * \param  ebt struct eb_t*
 * \return     int
 * 
 */
static int epoll_init(struct eb_t *ebt)
{
    int epfd;
    struct epoll_event *epevents;
    struct ebt_epoll *epo = (struct ebt_epoll *) ebt;

    epfd = epoll_create(EP_SIZE);
    if (epfd < 0)
    {
        err_msg("epoll_create failed!");
        return -1;
    }

    FD_CLOSEONEXEC(epfd);

    epevents = calloc(EP_SIZE , sizeof (struct epoll_event));
    if (epevents == NULL)
    {
        err_msg("malloc failed!");
        return -1;
    }

    epo->epfd     = epfd;
    epo->epevents = epevents;
    epo->epsz     = EP_SIZE;

    epo->readev = calloc(EP_SIZE, sizeof (struct ev *));
    if (epo->readev == NULL)
        return -1;

    epo->writev = calloc(EP_SIZE, sizeof (struct ev *));

    /* print ebt infomations */
    err_msg("ebt_epoll: [epfd=%d] [epevents=%p] [epsz=%d] [nfds=%d] [readev=%p] [writev=%p]",
        epo->epfd,
        epo->epevents,
        epo->epsz,
        epo->nfds,
        epo->readev,
        epo->writev
    );
    return 0;
}

/**
 * ebt事件循环
 * 
 * \param  ebt struct eb_t *
 * \param  tv  struct timeval *
 * \return     int
 * 
 */
static int epoll_loop(struct eb_t *ebt, const struct timeval *tv)
{
    struct ebt_epoll *epo = (struct ebt_epoll *) ebt;
    int cnt;    
    int timeout;

    timeout = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;

    cnt = epoll_wait(epo->epfd, epo->epevents, epo->epsz, timeout);

    if (cnt < 0)
        return errno == EINTR ? 0: -1;

    epo->nfds = cnt;

    int i;
    for (i = 0; i < cnt; ++i)
    {
        struct epoll_event *ev = epo->epevents + i;

        int fd = (uint32_t) ev->data.u64;
        int got = (ev->events & (EPOLLOUT | EPOLLERR | EPOLLHUP) ? E_WRITE : 0)
                | (ev->events & (EPOLLIN | EPOLLERR | EPOLLHUP) ? E_READ : 0);

        if (got & E_READ)
            eventq_in((struct ev *) epo->readev[fd]);

        if (got & E_WRITE)
            eventq_in((struct ev *) epo->writev[fd]);
    }

    return 0;
}


static int _resize(struct ebt_epoll *epo, int max)
{
    if (max > epo->epsz)
    {
        struct ev_io **readev, **writev;
        int newsz = epo->epsz;

        newsz <<= 1;

        readev = realloc(epo->readev, newsz * sizeof (struct ev *));
        if (readev == NULL)
            return -1;

        writev = realloc(epo->writev, newsz * sizeof (struct ev *));
        if (writev == NULL)
        {
            free(readev);
            return -1;
        }

        epo->readev = readev;
        epo->writev = writev;

        memset(readev + epo->epsz, 0, 
            (newsz - epo->epsz) * sizeof (struct ev *));
        memset(writev + epo->epsz, 0,
            (newsz - epo->epsz) * sizeof (struct ev *));

        epo->epsz = newsz;

        return 0;
    }

    return -1;
}

static int epoll_attach(struct eb_t *ebt, struct ev *e)
{
    struct epoll_event  ev;
    struct ebt_epoll    *epo     = (struct ebt_epoll *) ebt;
    struct ev_io        *evf     = (struct ev_io *) e;

    /* check for duplicate attachments*/
    if (epo->readev[evf->fd] != NULL && (epo->readev[evf->fd]->event.kide & e->kide))
    {
        errno = EBUSY;
        return -1;
    }
    if (epo->writev[evf->fd] != NULL && (epo->writev[evf->fd]->event.kide & e->kide))
    {
        errno = EBUSY;
        return -1;
    }

    /* make room for this event ?*/
    if ((unsigned int) evf->fd >= epo->epsz && _resize(epo, evf->fd))
        return -1;

    int op     = EPOLL_CTL_ADD;
    int events = 0;

    events = (evf->event.kide & E_READ ? EPOLLIN: 0) | (evf->event.kide & E_WRITE ? EPOLLOUT : 0);

    if (epo->readev[evf->fd] != NULL)
    {
        events |= EPOLLIN;
        op = EPOLL_CTL_MOD;
    }
    if (epo->writev[evf->fd] != NULL)
    {
        events |= EPOLLOUT;
        op = EPOLL_CTL_MOD;
    }

    ev.data.u64 = evf->fd;
    ev.events   = events;

    if (epoll_ctl(epo->epfd, op, evf->fd, &ev) == -1)
    {
        err_msg("epoll_ctl error: [errno=%d] [errstr=%s]", errno, strerror(errno));
        return -1;
    }

    //debug info
    err_debug ("ev: [ev=%p] [fd=%d] [op=%s] [events=%s] [kide=%s] [cb=%p]", 
        e, 
        evf->fd, 
        op == EPOLL_CTL_ADD  ? "EPOLL_CTL_ADD": "EPOLL_CTL_MOD", 

        ev.events ^ (EPOLLIN | EPOLLOUT) ? 
            (ev.events & EPOLLOUT ? "EPOLLOUT" : 
            (ev.events & EPOLLIN) ? "EPOLLIN": "") : "EPOLLIN|EPOLLOUT",

        e->kide ^ (E_READ | E_WRITE) ? 
            (e->kide & E_WRITE ? "E_WRITE": 
            (e->kide & E_READ) ? "E_READ" : "") : "E_READ|E_WRITE",

        e->cb);

    if (evf->event.kide & E_READ)
        epo->readev[evf->fd] = evf;

    if (evf->event.kide & E_WRITE)
        epo->writev[evf->fd] = evf;

    return 0;
}

static int epoll_detach(struct eb_t *ebt, struct ev *e)
{
    struct epoll_event  ev;
    struct ebt_epoll    *epo = (struct ebt_epoll *) ebt;
    struct ev_io        *evf = (struct ev_io *) e;

    int events = (e->kide & E_READ ? EPOLLIN : 0) | (e->kide & E_WRITE ? EPOLLOUT : 0);
    int op     = EPOLL_CTL_DEL;
    int rd     = 1; 
    int wd     = 1;

    if (events ^ (EPOLLIN|EPOLLOUT))
    {
        if ((events & EPOLLIN) && epo->writev[evf->fd] != NULL)
        {
            wd     = 0;
            events = EPOLLOUT;
            op     = EPOLL_CTL_MOD;
        }
        else if ((events & EPOLLOUT) && epo->readev[evf->fd] != NULL)
        {
            rd     = 0;
            events = EPOLLIN;
            op     = EPOLL_CTL_MOD;
        }
    }

    ev.events   = events;
    ev.data.u64 = evf->fd;

    if (epoll_ctl(epo->epfd, op, evf->fd, &ev) < 0)
    {
        err_msg("epoll_ctl error: [errno=%d] [errstr=%s]", errno, strerror(errno));
        return -1;
    }

    //debug info
    err_debug ("ev: [ev=%p] [fd=%d] [op=%s] [events=%s]",
        e,
        evf->fd,
        op == EPOLL_CTL_DEL ? "EPOLL_CTL_DEL": "EPOLL_CTL_MOD",

        ev.events ^ (EPOLLIN | EPOLLOUT) ? 
            (ev.events & EPOLLOUT ? "EPOLLOUT" : 
            (ev.events & EPOLLIN) ? "EPOLLIN": "") : "EPOLLIN|EPOLLOUT"
    );

    if (rd) epo->readev[evf->fd] = NULL;
    if (wd) epo->writev[evf->fd] = NULL;

    return 0;
}

static int epoll_free(struct eb_t *ebt)
{
    struct ebt_epoll *epo = (struct ebt_epoll *) ebt;

    free(epo->readev);
    free(epo->writev);
    free(epo->epevents);
    close(epo->epfd);

    return 0;
}

/******************************************************************/
/* event functions                                                */
/******************************************************************/
void ev_init(struct ev *e, enum e_kide kide, e_cb_t *cb, void *arg)
{
    e->kide = kide;
    e->cb   = cb;
    e->arg  = arg;  
}

struct ev * ev_read(int fd, e_cb_t *cb, void *arg)
{
    struct ev_io *event;    
    event = calloc(1, sizeof (*event));

    if (event == NULL)
    {
        err_msg("calloc failed!");
        return NULL;
    }

    ev_init((struct ev *) event, E_READ, cb, arg);
    event->fd = fd;

    return (struct ev *) event;
}

struct ev * ev_write(int fd, e_cb_t *cb, void *arg)
{
    struct ev_io *event;
    event = calloc(1, sizeof (*event));

    if (event == NULL)
    {
        err_msg("calloc failed!");
        return NULL;
    }

    ev_init((struct ev *) event, E_WRITE, cb, arg);
    event->fd = fd;

    return (struct ev *) event;
}

struct ev * ev_timer(const struct timeval *tv, e_cb_t *cb, void *arg)
{
    struct ev_timer *event;
    event = calloc(1, sizeof (*event));

    if (event == NULL)
    {
        err_msg("calloc failed!");
        return NULL;
    }

    ev_init((struct ev *) event, E_TIMER, cb, arg);
    event->tv = *tv;

    return (struct ev *) event;
}

struct ev * ev_signal(int sig, e_cb_t *cb, void *arg)
{
    struct ev_signal *event;
    event = calloc(1, sizeof (*event));

    if (event == NULL)
    {
        err_msg("calloc failed!");
        return NULL;
    }

    ev_init((struct ev *) event, E_SIGNAL, cb, arg);
    event->signal = sig;

    return (struct ev *) event;
}

struct ev * ev_child(pid_t pid, e_cb_t *cb, void *arg)
{
    struct ev_child *event;
    event = calloc(1, sizeof (*event));

    if (event == NULL)
    {
        err_msg("calloc failed!");
        return NULL;
    }

    ev_init((struct ev *) event,E_CHILD, cb, arg);
    event->child = pid;

    return (struct ev *) event;
}

struct ev * ev_flag(int flag, e_cb_t *cb, void *arg)
{
    struct ev_flag *event;
    event = calloc(1, sizeof (*event));

    if (event == NULL)
    {
        err_msg("calloc failed!");
        return NULL;
    }
    ev_init((struct ev *) event, E_FLAG, cb, arg);
    event->flag = flag;

    return (struct ev *) event;
}

void ev_free(struct ev *e)
{
    free(e);
}

/******************************************************************/
/* timer functions                                                */
/******************************************************************/
static int timer_insert(struct eb_t *ebt, struct ev_timer *evt)
{
    if (evt->event.kide & E_TIMER)
    {
        RB_INSERT(timer_tree, &ebt->timers, evt);
        ebt->numtimers++;
        return 0;
    }
    return -1;
}

static int timer_remove(struct eb_t *ebt, struct ev_timer *evt)
{
    if (evt->event.kide & E_TIMER)
    {
        RB_REMOVE(timer_tree, &ebt->timers, evt);
        ebt->numtimers--;
        return 0;
    }
    return -1;
}

static int timer_reset(struct ev_timer *evt)
{
    if (!(evt->event.kide & E_TIMER))
        return -1;

    if (timer_remove(evt->event.ebt, evt) < 0)  
        return -1;

    struct timeval zero = {0, 0};

    timeradd(&evt->remain, &evt->tv, &evt->remain);
    if (timercmp(&evt->remain, &zero, <) < 0)
        evt->remain = zero;

    if (timer_insert(evt->event.ebt, evt) < 0)
        return -1;

    return 0;
}

static int timer_attach(struct eb_t *ebt, struct ev_timer *evt)
{
    struct ev_timer *ev_t;
    if (ebt->timerdebt.tv_sec != 0 || ebt->timerdebt.tv_usec != 0)
    {
        RB_FOREACH(ev_t, timer_tree, &ebt->timers)
        {
            timersub(&ev_t->remain, &ebt->timerdebt, &ev_t->remain);
        }

        timerclear(&ebt->timerdebt);
    }

    //刚加入队列时, 定时器剩余时间等于定时时间
    evt->remain = evt->tv;
    return timer_insert(ebt, evt);
}

static int timer_detach(struct eb_t *ebt, struct ev_timer *evt)
{
    if (! (evt->event.kide & E_TIMER))
        return -1;

    if (timer_remove(ebt, evt) < 0)
        return -1;

    if (ebt->numtimers == 0)
        timerclear(&ebt->timerdebt);

    return 0;
}

static int wait_for_events(struct eb_t *ebt, const struct timeval *start, struct timeval *end)
{
    struct ev_timer *timer = NULL;
    struct ev_flag *evf;
    struct timeval tv;
    unsigned int i;

    /* 如果存在用户自定义事件, 也放到dispatchq队列 */
    TAILQ_FOREACH(evf, &ebt->flags, flags)  
    {
        if (evf->flag)
        {
            if (eventq_in((struct ev *) evf) == 0)
                err_debug("flag events in dispatchq: [ev=%p] [flag=%d]", evf, evf->flag);
        }
    }

    if (TAILQ_EMPTY(&ebt->dispatchq) && ebt->numtimers == 0)
        return 0;

    /* 定时器处理 */
    if (ebt->numtimers != 0)
    {
        static const struct timeval zero = {0, 0};

        timer = RB_MIN(timer_tree, &ebt->timers);
        timersub(&timer->remain, &ebt->timerdebt, &tv);

        /* 反应堆dispatch */
        if (timercmp(&tv, &zero, >=) && ebt->ebo->loop(ebt, &tv) < 0)
            return -1;
    }
    else
    {
        if (ebt->ebo->loop(ebt, NULL) < 0)
            return -1;
    }

    gettimeofday(end, NULL);

    if (timer == NULL)
        return 0;
    timersub(end, start, &tv);
    timeradd(&ebt->timerdebt, &tv, &ebt->timerdebt);
    if (timercmp(&ebt->timerdebt, &timer->remain, <))
        return 0;

    /* 更新所有定时器 */
    RB_FOREACH(timer, timer_tree, &ebt->timers)
    {
        timersub(&timer->remain, &ebt->timerdebt, &timer->remain);

        if (timer->remain.tv_sec < 0 || (timer->remain.tv_sec == 0 && timer->remain.tv_usec <= 0))
        {
            if (eventq_in((struct ev *) timer) == 0)
                err_debug("timer event in dispatchq: [timer=%p] [tv.tv_sec=%d] [tv.tv_usec=%d]", 
                    timer, timer->tv.tv_sec, timer->tv.tv_usec);
        }
    }

    timerclear(&ebt->timerdebt);

    return 0;
}

/**
 * 
 * 触发队列中的就绪事件
 * 
 * @param ebt struct eb_t*
 * 
 */
static void dispatch_queue(struct eb_t *ebt)
{
    struct ev *e;

    if (!TAILQ_EMPTY(&ebt->dispatchq))
    {
        enum e_opt opt;
        int num;
        for (e = TAILQ_FIRST(&ebt->dispatchq); e; e = TAILQ_FIRST(&ebt->dispatchq))
        {
            /* 从队列移除 */
            TAILQ_REMOVE(&ebt->dispatchq, e, dispatchq);
            e->opt &= ~E_QUEUE;
            
            switch (e->kide)
            {
                case E_READ:
                case E_WRITE:
                    num = ((struct ev_io *) e)->fd;
                    break;

                default:
                    num = -1;
                    break;
            }

            /* 如果是一次性的事件? 立刻移除 */
            opt = e->opt;
            e->opt &= ~ E_FREE;
            if (e->opt & E_ONCE)
                ev_detach(e);

            /* 调用事件处理函数 */
            if (e != NULL)
                e->cb(num, e->arg);

            /* 如果事件处理函数中, 删除了该事件? */
            if (!ev_attach(e))
                return;

            /* 如果事件从队列被删除了, 要释放其内存空间? */
            if (opt & (E_ONCE | E_FREE))
                ev_free(e);
            else if ( e->kide == E_TIMER)
                //定时器被触发开后, 重新设置并投递定时器队列
                timer_reset((struct ev_timer *) e);
        }
    }
}

/******************************************************************/
/* event queue function                                           */
/******************************************************************/

/**
 * 注册事件e到ebt例程
 * 
 * \param  e   struct ev*
 * \param  ebt struct eb_t*
 * \return     int
 * 
 */
int ev_attach(struct ev *e, struct eb_t *ebt)
{
    if (!(e->kide & ebt->kides))
    {
        errno = ENOTSUP;
        return -1;
    }

    if (e->ebt != NULL)
    {
        errno = EBUSY;
        return -1;
    }

    switch (e->kide)
    {
        case E_TIMER:
            if (timer_attach(ebt, (struct ev_timer *) e) < 0)
                return -1;
            break;

        case E_FLAG:
            TAILQ_INSERT_TAIL(&ebt->flags, (struct ev_flag *) e, flags);
            break;

        default:
            if (ebt->ebo->attach(ebt, e) < 0)
                return -1;
            break;
    }
    ebt->num++;
    e->ebt= ebt;

    return 0;
}

int ev_detach(struct ev *e, struct eb_t *ebt)
{
    if (e->ebt == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    switch (e->kide)
    {
        case E_TIMER:
            if (timer_detach(ebt, (struct ev_timer *) e) < 0)
                return -1;
            break;

        case E_FLAG:
            TAILQ_REMOVE(&ebt->flags, (struct ev_flag *) e, flags);
            break;

        default:
            if (ebt->ebo->detach(ebt, e) < 0)
                return -1;
            break;
    }

    if (e->opt & E_QUEUE)
    {
        TAILQ_REMOVE(&ebt->dispatchq, e, dispatchq);
        e->opt &= ~E_QUEUE;
    }

    ebt->num--;
    e->ebt = NULL;

    if (e->opt & E_FREE)
        ev_free(e);

    return 0;
}

/**
 * 将事件e加入到dispatchq队列
 * 
 * \param e struct ev*
 * 
 */
int eventq_in(struct ev *e)
{
    if (e->opt & E_QUEUE)
        return -1;

    TAILQ_INSERT_TAIL(&e->ebt->dispatchq, e, dispatchq);
    e->opt |= E_QUEUE;

    return 0;
}

/******************************************************************/
/* ebt functions                                                  */
/******************************************************************/
struct eb_t * ebt_new(enum e_kide kides)
{
    static const struct eb_o *ebo_map [] = {
        &ebo_epoll,
        NULL
    };
    struct eb_t *ebt;
    const struct eb_o **ebo;

    /* 寻找对应支持事伯的操作实例 */
    for (ebo = ebo_map; *ebo; ebo++)
    {
        if ((((*ebo)->kides | E_TIMER | E_FLAG) & kides ) != kides)
            continue;

        ebt = malloc((*ebo)->ebtsz);
        if (ebt == NULL)
            return NULL;

        memset(ebt, 0, (*ebo)->ebtsz);
        ebt->kides = kides;
        ebt->ebo   = *ebo;

        //初始化事件队列
        TAILQ_INIT(&ebt->flags);
        TAILQ_INIT(&ebt->dispatchq);
        RB_INIT(&ebt->timers);

        /* 初始化ebt派生结构 */
        if (ebt->ebo->init(ebt) < 0)
        {
            free(ebt);
            continue;
        }

        return ebt;
    }
    errno = ENOTSUP;

    return NULL;
}

int ebt_loop(struct eb_t *ebt)
{
    struct timeval tv[2];
    int i, ret;

    ebt->broken = 0;

    /* 如果loop存在construct, 先运行 */
    if (ebt->ebo->construct != NULL && ebt->ebo->construct(ebt) < 0)
        return -1;

    i = 0;
    gettimeofday(tv + i, NULL);

    while (ebt->num > 0 && !ebt->broken)
    {
        /* 等待事件发生 */
        ret = wait_for_events(ebt, tv + i, tv + (!i));
        if (ret < 0)
            break;

        /* 激活队列中的就绪事件 */
        dispatch_queue(ebt);

        i = !i;
    }

    /* ebt循环结束后, 析构 */
    if (ebt->ebo->destruct != NULL && ebt->ebo->destruct(ebt) < 0)
        return -1;

    return ret;
}

void ebt_break(struct eb_t *ebt)
{
    ebt->broken = 1;    
}

void ebt_free(struct eb_t *ebt)
{
    ebt->ebo->free(ebt);
    free(ebt);
}
/******************************************************************/
/* network                                                        */
/******************************************************************/
enum e_type
{
    E_START     = 0,
    E_CONNECT   = 1,
    E_RECEIVE   = 2,
    E_TIMER     = 3
    E_CLOSE     = 4,
    E_SHUTDOWN  = 5,
    E_ERROR     = 6
};

enum s_type
{
    E_CLOSED     = 0,
    E_CLOSING    = 1,
    E_CONNECTING = 2,
    E_CONNECTED  = 3,
    E_RECEIVING  = 4,
    E_RECEIVED   = 5,
    E_STARTED    = 6,
    E_STARTING   = 7,
    E_SHUTDOWNED = 8
};

struct data_buffer;

#define E_MAX_ETYPE       32
#define E_BACK_LOG        512
#define E_TIMEOUT_SEC     0
#define E_TIMEOUT_USEC    3000000
#define E_NUM_REACTORS    4
#define E_NUM_FACTORIES   2
#define E_MASTER_REACTOR  E_NUM_REACTORS

struct EventData
{
    int fd;
    int type;
    uint16_t len;
    uint16_t from_reactor_id;
    char data[2048];
};

struct sa
{
    socklen_t len;
    union {
        struct sockaddr sa;
        struct sockaddr_in sin;
    } u;
#ifdef WITH_IPV6
    struct sockaddr_in6 sin6;
#endif
};

struct reactor
{
    struct eb_t        *base;
    struct factory     *factory
    void               *ptr;
    int                 reactor_id;
    int                 status; 
};

struct child_reactor
{
    struct reactor      reactor;
    struct data_buffer  *buf;
};

struct master_reactor
{
    struct reactor      reactor;
    pthread_t           thread_id;
};

struct factory
{
    int     factory_id;
    int     status;
    int     max_request;
    void    *ptr;

    int (*task)(struct EventData *);
};

struct settings
{
    uint16_t backlog;
    uint8_t daemonize;
    uint8_t num_reactors;
    uint8_t num_factories;

    int sock_cli_bufsize;   //client的socket缓存设置
    int sock_srv_bufsize;   //server的socket缓存设置

    int max_conn;
    int max_request;
    int timeout_sec;
    int timeout_usec;
};

typedef int (*e_handle_t)(struct EventData *);

struct ebt_srv
{
    struct settings         settings;           //应用服务配置
    struct master_reactor   mreactor;           //主反应堆
    struct thread_pool      reactor_pool;       //子反应堆线程池
    struct thread_pool      factory_pool;       //任务调度线程池
    int                     pipe[2];            //通信管道
    int                     sfd;                //服务器套接字
    e_handle_t              handles[E_MAX_ETYPE];//注册到服务对象的回调 
};

#define ebt_srv_get_thread(srv, w, n)   (srv->w.threads[n])
#define ebt_srv_get_param(srv, w, n)    (srv->w.params[n])
#define ebt_srv_get_reactor(srv, n)     (struct reactor *) ebt_srv_get_thread(srv, reactor_pool, n).data.ptr
#define ebt_srv_get_factory(srv, n)     (struct factory *) ebt_srv_get_reactor(srv, factory_pool, n).data.ptr

static int ebt_srv_write(int fd, char *buf, int len)
{
    int nwrite = 0;
    int total_len = 0;

    while (total_len != len)
    {
        nwrite = write(fd, buf, len - total_len);
        if (nwrite == -1)
            return total_len;

        if (nwrite == -1)
        {
            if (errno == EINTR)
                continue;
            else if (errno == EAGAIN)
            {
                sleep(1);
                continue;
            }
            else
                return -1;
        }
        total_len += nwrite;
        buf += nwrite;
    }

    return total_len;
}

static ebt_srv_read(int fd, char *buf, int len)
{
    int n = 0, nread;

    while (1)
    {
        nread = read(fd, buf + n, len -n);
        if (nread < 0)
        {
            if (errno == EINTR) continue
            break;
        }
        else if (nread == 0)
        {
            return 0;
        }
        else
        {
            n += nread;
            if (n == len) break;
            continue;
        }
    }
    return n;
}

static void ebt_srv_settings_init(struct settings *settings)
{
    settings->backlog       = E_BACK_LOG;
    settings->daemonize     = 0;
    settings->num_reactors  = E_NUM_REACTORS;
    settings->num_factories = E_NUM_FACTORIES;
    
    settings->timeout_sec   = E_TIMEOUT_SEC;
    settings->timeout_usec  = E_TIMEOUT_USEC;    
}


static int 
ebt_srv_reactors_init(struct ebt_srv *srv)
{
    int i;
    int size = srv->settings.num_reactors;
    struct child_reactor *reactors;

    reactors = calloc(size, sizeof * reactors);

    if (reactors == NULL)
    {
        err_msg("can't allocate memory for reactor pool!");
        return -1;
    }

    //初始化线程池, 并为线程分配反应堆
    thread_pool_init(&srv->reactor_pool, size);

    for (i = 0; i < size; i++)
    {
        struct reactor *reactor = ebt_srv_get_reactor(srv, i);

        reactor = &reactors[i];
        reactor->reactor_id = i;
        reactor->ptr = srv;

        //TODO: 创建data_buf   
    }

    return 0;
}

static int
ebt_srv_factories_init(struct ebt_srv *srv)
{
    int i;
    int size = srv->settings.num_factories;
    struct factory *factories;

    factories = calloc(size, sizeof * factories);

    if (factories == NULL)
    {
        err_msg("can't allocate memory for factory  pool!");
        return -1;
    }

    thread_pool_init(&srv->factory_pool, size);

    //为线程分配任务调度器实例
    for (i = 0; i < size; i++)
    {
        struct factory *factory = ebt_srv_get_factory(srv, i);

        factory = factories[i];
        factory->factory_id = i;
        factory->ptr = srv;
        factory->task = srv->handles[E_RECEIVE];

    }

    return 0;
}

static void* ebt_srv_poll_routine(void *arg)
{
    int n;
    struct thread_param *param;
    struct thread_entity *thread;
    struct reactor *reactor;
    struct ebt_srv *srv;
    struct ev *ev;

    param = (struct thread_param *) arg;
    n     = param->id;
    srv   = param->data;

    reactor = ebt_srv_get_reactor(srv, n);
    reactor->base = ebt_new(E_READ | E_WRITE);

    thread = ebt_srv_get_thread(srv, reactor_pool, n);

    ev = ev_read(thread->notify_recv_fd, ebt_srv_nofitify, srv);

    ev_attach(ebt, ev);
    ebt_loop(reactor->base);
    ebt_free(reactor->base);

    return NULL;
}

static int ebt_srv_poll_start(struct ebt_srv *srv)
{
    int i;
    int size = srv->settings.num_reactors;

    for (i = 0; i < size; i++)
    {
        //TODO:
    }
    thread_pool_run(srv->reactor_pool, ebt_srv_poll_routine);

    return 0;
}

static void ebt_srv_poll_event_process(short num, struct ebt_srv *srv)
{
    int fd = num;
    int n;
    struct factory *factory;
    struct EventData edata;

    n = ebt_srv_read(fd, edata.data, sizeof(edata.data));

    if (n < 0)
        return;
    else if (n == 0)
        ebt_srv_close();
    else
    {
        
    }
}

static int ebt_srv_factory_start(struct factory *factory);
static int ebt_srv_factory_routine(struct factory *factory);
static int ebt_srv_factory_event_process(short num, struct *factory);

static void ebt_srv_accept(short sfd, struct ebt_srv *srv)
{
    int r, n, new_fd;
    struct ev *ev; 
    struct EventData edata;
    struct child_reactor *reactor;

    new_fd = accept(sfd, &cli_addr, sizeof(cli_addr));

    //取模散列
    n = new_fd % srv->settings.num_reactors;
    reactor = ebt_srv_get_reactor(srv, n);

    //将new_fd添加到子反应堆
    ev = ev_read(new_fd, void(*)(short, void *) ebt_srv_poll_event_process, srv);
    r  = ev_attach(reactor->ebt, ev);

    if (r < 0)
        err_msg("masterThread add event fail [cli_fd=%d] [reactor_id=%d]!", new_fd, reactor->reactor_id)

    edata.fd              = new_fd;
    edata.type            = E_CONNECTED;
    edata.len             = sizeof(new_fd);
    edata.from_reactor_id = reactor.reactor_id;
    edata.data            = "client connected";

    //回调connent方法
    srv->handles[E_CONNECT](&edata);
}

/**
 * 创建一个srv实例
 */
int ebt_srv_create(struct ebt_srv *srv)
{
    int r = 0;

    memset(srv, 0, sizeof * srv);
    //初始化配置信息
    ebt_srv_settings_init(&srv->settings);

    if (pipe(srv->pipe) < 0)
    {
        err_msg("can't create pipe!");
        return -1;
    }

    //初始化反应堆线程池和任务调度线程池
    r = ebt_srv_reactors_init(srv);
    if (r < 0)
    {
        err_msg("can't create child reactor thread pool!");
        return -1;
    }

    r = ebt_srv_factories_init(srv);
    if (r < 0)
    {
        err_msg("can't create factory thread pool!");
        return -1;
    }

    return 0; 
}

int ebt_srv_listen(struct ebt_srv *srv, short port)
{
    int sfd, on =1, af;

#ifdef WITH_IPV6
    af = PF_INET6;
#else
    af = PF_INET;
#endif

    if (((sfd) = socket(af, SOCK_STREAM, 6)) == -1)
        err_msg("create socket fail: %s", strerror(errno));

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

    if (bind(sfd, &sa.u.sa, sa.len) < 0)
        err_msg("bind: [af=%d] [port=%d] [err=%s]", af, port, strerror(errno));

    (void) listen(sfd, 16);

    srv->sfd = sfd;

    return 0;
}

/**
 * 启动一个srv实例
 */
int ebt_srv_start(struct ebt_srv *srv)
{
    struct EventData edata;
    struct timeval tv;
    struct eb_t *mbase;
    struct ev *ev[2];
    int r = 0, sfd;

    //作为守护进程
    if (srv->settings.daemonize > 0)
    {
        if (daemon(0, 0) < 0)
            return -1;
    }

    r = ebt_srv_poll_start(srv);
    if (r < 0)
        err_exit("reactor threads start polling fail.");

    r = ebt_srv_factory_start(&srv->factory_pool);
    if (r < 0)
        err_exit("factory threads start working fail.");

    mbase = ebt_new(E_READ | E_WRITE);
    if (mbase == NULL)
        err_exit("can't create master reactor for master thread.");

    srv->mreactor.base = mbase;
    srv->mreactor.base.reactor_id = E_MASTER_REACTOR; //标记为主master反应堆
    srv->mreactor.thread_id = pthread_self();

    if (srv->sfd <= 0)
        return -1;

    ev[0] = ev_read(sfd, void(*)(short, void*) ebt_srv_accept, srv);
    ev[1] = ev_read(srv->pipe[0], void(*)(short, void*) ebt_srv_close, srv);

    ev_attach(mbase, ev[0]);
    ev_attach(mbase, ev[1]);

    edata.fd = sfd;
    edata.from_reactor_id = mbase.reactor_id;
    edata.type = E_STARTED;

    if (srv->handles[E_START] != NULL)
    {
        srv->handles[E_START](&edata);
    }

    ebt_loop(ebt);

    if (srv->handles[E_SHUTDOWN] != NULL)
    {
        edata.type = E_SHUTDOWNED;
        srv->handles[E_SHUTDOWN](&edata);
    }

    return 0;
}

int ebt_srv_on(struct ebt_srv *srv, enum e_type type, e_handle_t cb)
{
    if (type >= E_MAX_ETYPE)
        return -1;
    else
        srv->handles[type] = cb;

    return 0;
}

void ebt_srv_free(struct ebt_srv *srv)
{
    
}

/******************************************************************/
/* test                                                           */
/******************************************************************/
struct ev_param 
{
    char buf[256];
    struct timeval tv;
};

struct item_bz
{
    char buf[128];
    int a;
    struct item_bz *next;
};

void cb (short num, void *arg)
{
    err_msg("cb is runnig: [fd=%d] [ev_io=%p]", num, arg);
}

void tcb(short num, void *arg)
{

    // struct ev_timer *evt = (struct ev_timer *) arg;
    // err_msg("tcb was invoke: [tv.tv_sec=%d] [tv.tv_usec=%d] [ev_timer=%p]", evt->remain.tv_sec, evt->remain.tv_usec, arg);
    // err_msg("tcb arg: [arg=%s]", (char *)(arg));

    struct ev_param *evp = (struct ev_param *) arg;
    err_msg("tcb was invoke: [ev_param=%p] [tv.tv_sec=%d] [tv.tv_usec=%d] [buf=%s]", arg, evp->tv.tv_sec, evp->tv.tv_usec, evp->buf);

    err_msg("tcb was invoke: [num=%d]", num);
}

void tcb1(short num, void *arg)
{
    err_msg("tcb1 was invoked: [num=%d]", num);
}

void fcb(short num, void *arg)
{
    err_msg("fcb was invoke: [num=%d] [arg=%p]", num, arg);
}

void printEbt(struct eb_t *ebt)
{
    printf("\n\n\n");

    struct ebt_epoll *epo = (struct ebt_epoll *) ebt;   
    err_msg("ebt -> epo info: [epo=%p] [ebo=%p] [kides=%x] [timer_tree=%p] [dispatchq=%p] [flags=%p]", epo, ebt->ebo, ebt->kides, &ebt->timers, &ebt->dispatchq, &ebt->flags);

    //print dispatchq
    if(TAILQ_EMPTY(&ebt->dispatchq))
        err_msg("dispatchq: it's empty!");
    else
    {
        struct ev *e;
        TAILQ_FOREACH(e, &ebt->dispatchq, dispatchq)
        {
            err_msg("dispatchq item: [adr=%p] [kide=%x] [opt=%x]", e, e->kide, e->opt);
        }
    }

    //print readev
    int i, numq = 0;
    struct ev_io *evi;
    for (i = 0; i < epo->epsz; i++)
    {
        if (epo->readev[i] != NULL)
        {
            evi = (struct ev_io *) epo->readev[i];
            err_msg("readev item: [adr=%p] [fd=%d] [pos=%d]", evi, evi->fd, i);
        }
    }
    //print writev
    for (i = 0; i < epo->epsz; i++)
    {
        if (epo->writev[i] != NULL)
        {
            evi = (struct ev_io *) epo->writev[i];
            err_msg("writev item: [ard=%p] [fd=%d] [pos=%d]", evi, evi->fd, i);
        }
    }

    //print timer tree info
    if (RB_EMPTY(&ebt->timers))
        err_msg("timer tree: it's empty!");
    else
    {
        struct ev_timer *evt;
        RB_FOREACH(evt, timer_tree, &ebt->timers)
        {
            err_msg("timer tree item: [adr=%p] [remain.tv_sec=%d] [remain.tv_usec=%d]", evt, evt->remain.tv_sec, evt->remain.tv_usec);  
        }
    }

    //print flags queue info
    if (TAILQ_EMPTY(&ebt->flags))
        err_msg("flagsq: it's empty!");
    else
    {
        struct ev_flag *e;
        TAILQ_FOREACH(e, &ebt->flags, flags)
        {
            err_msg("flagsq item: [adr=%p] [flag=%d]", e, e->flag);
        }
    }

    err_msg("ebt total ev nums: [num=%d]", ebt->num);
    err_msg("ebt total timer nums: [numtimers=%d]", ebt->numtimers);
    err_msg("ebt total dispatchq nums: [nums=%d]", numq);

    printf("\n\n\n");
}

void *thread_route(void *arg)
{
    pthread_t thread_id = pthread_self();
    struct thread_param *param = (struct thread_param *) arg;
    struct thread_pool *pool = param->data;
    struct item_bz tm_bz;
    int ret;

    err_msg("thread [%d] is starting to work", thread_id);

    while(1)
    {
        pthread_mutex_lock(&pool->mutex);

        if (pool->shutdown)
        {
            pthread_mutex_unlock(&pool->mutex);
            err_msg("thread[%d] will exit", (int)pool->threads[param->id].thread_id);
            pthread_exit(NULL);
        }

        if (pool->num_tasks == 0)
            pthread_cond_wait(&pool->cond, &pool->mutex);

        ret = cqueue_shift(pool->cq, &tm_bz, sizeof (struct item_bz));

        pthread_mutex_unlock(&pool->mutex);

        if (ret >= 0)
        {
            _u32_t *num_tasks = &pool->num_tasks;
            atom_sub(num_tasks, 1);

            err_msg("thread[%d] work on task: [id=%d] [buf=%s]", (int)pool->threads[param->id].thread_id, tm_bz.a, tm_bz.buf);
        }
    }

    pthread_exit(NULL);
    return NULL;
}

int main(int argc, char *argv[])
{
    struct eb_t ebt;
    struct ev_timer *evt;

    evt = malloc (10 * sizeof (*evt));

    if (evt == NULL)
        err_msg("malloc failed");

    srand((unsigned)time(NULL));
    int i, num;
    for (i = 0; i < 10; i++)
    {
        num = rand() % 11;
        memset(&evt[i].event, 0, sizeof (struct ev));
        evt[i].event.kide     = E_TIMER;
        evt[i].event.ebt      = &ebt;
        evt[i].remain.tv_sec  = num;
        evt[i].remain.tv_usec = 0;
    }

    ebt.numtimers = 0;
    RB_INIT(&ebt.timers);

    for (i =0; i < 10; i++)
    {
        err_msg("ev_timer: [adr=%p] [kide=%x] [tv_sec=%d] [tv_usec=%d]", &evt[i], evt[i].event.kide, evt[i].remain.tv_sec, evt[i].remain.tv_usec);
        timer_insert(&ebt, &evt[i]);
    }

    printf("\n");

    struct ev_timer *ev_t;
    RB_FOREACH(ev_t, timer_tree, &ebt.timers)
    {
        err_msg("ev_timer: [adr=%p] [kide=%x] [tv_sec=%d] [tv_usec=%d]", ev_t, ev_t->event.kide, ev_t->remain.tv_sec, ev_t->remain.tv_usec);
    }

    printf ("\n");
    err_msg("current total timers [numtimers=%d]", ebt.numtimers);
    printf ("\n");

    struct ev_timer *tmp = &evt[3];
    timer_remove(&ebt, tmp);

    tmp = &evt[4];
    tmp->remain.tv_sec  = 14;
    tmp->remain.tv_usec = 0;
    timer_reset(tmp);

    ev_t = NULL;
    RB_FOREACH(ev_t, timer_tree, &ebt.timers)
    {
        err_msg("ev_timer: [adr=%p] [kide=%x] [tv_sec=%d] [tv_usec=%d]", ev_t, ev_t->event.kide, ev_t->remain.tv_sec, ev_t->remain.tv_usec);
    }

    printf ("\n");
    err_msg("current total timers [numtimers=%d]", ebt.numtimers);
    printf ("\n");

    free(evt);


    struct eb_t *eb = ebt_new(E_READ|E_WRITE);

    for (i = 0; i < 10; i++)
    {
        struct ev *io = ev_read(i, cb, io);
        io->ebt = eb;   
        eventq_in(io);
        err_msg("add to q: [fd=%d] [evi=%p]", i, io);
    }

    printf("\n");

    //print io
    struct ev *e = NULL;
    TAILQ_FOREACH(e, &eb->dispatchq, dispatchq)
    {
        struct ev_io *evi = (struct ev_io *) e;

        e->cb(evi->fd, evi);    
    }

    printf("\n");

    //release io
    while (e = TAILQ_FIRST(&eb->dispatchq))
    {
        TAILQ_REMOVE(&eb->dispatchq, e, dispatchq);

        struct ev_io *evi = (struct ev_io *) e;
        err_msg("remove fd event: [fd=%d]", evi->fd);

        if (evi)
            free(evi);
    }

    ebt_free(eb);


    printf ("\n\n");

    struct ev *t1, *t2, *t3, *t4;
    char *buf ="###__---%%%%||||bbbbbbbbbb";
    int j, k, ret;

    struct eb_t *nebt = ebt_new(E_READ | E_WRITE | E_TIMER | E_FLAG);
    struct timeval tv;
    printEbt(nebt);

    //test timer
    struct ev *et;
    for (j = 0; j < 10; j++)
    {
        tv.tv_sec     = rand() % 15 + 5;
        tv.tv_usec    = 0;
        struct ev *et = ev_timer(&tv, tcb1, &tv);
        ret           = ev_attach(et, nebt);

        //手动入队测试
        eventq_in(et);

        //保存第5个
        if (j == 4)
            t1 = et;
    }

    //test io fd
    struct ev *ef;

    // ef = ev_write(0, fcb, buf);
    // ef->kide |= E_READ;
    // ret = ev_attach(ef, nebt);

    for (j = 0; j < 10; j++)
    {
        ef  = ev_write(j, fcb, buf);
        ret = ev_attach(ef, nebt);

        if (j == 0)
            t4 = ef;
    }

    for (j = 0; j < 10; j++)
    {
        ef  = ev_read(j, fcb, buf);
        ret = ev_attach(ef, nebt);

        if (j == 0)
            t3 = ef;
    }

    //test flag
    struct ev *evf;
    for (j = 0; j < 10; j++)
    {
        evf = ev_flag(j + 1, fcb, buf);
        ret = ev_attach(evf, nebt);

        if (j == 2)
            t2 = evf;
    }

    printEbt(nebt); 

    ev_detach(t1, nebt);
    ev_detach(t2, nebt);
    ev_detach(t3, nebt);
    ev_detach(t4, nebt);

    dispatch_queue(nebt);

    printEbt(nebt);

    err_msg("ret =%d errno=%d errstr=%s", ret, errno, strerror(errno));

    ebt_free(nebt);



    // // test event loop
    // struct eb_t *ebt1 = ebt_new(E_READ | E_WRITE | E_TIMER);

    // for (j = 0; j < 10; j++)
    // {
    //  struct ev_param *dt = calloc(1, sizeof(struct ev_param));
    //  memcpy(dt->buf, buf, strlen(buf) + 1);
    //  tv.tv_sec     = rand() % 15 + 5;
    //  tv.tv_usec    = 0;
    //  dt->tv        = tv;

    //  struct ev *et = ev_timer(&tv, tcb, dt);
    //  ret           = ev_attach(et, ebt1);
    // }

    // printEbt(ebt1);

    // ebt_loop(ebt1);

    // printEbt(ebt1);

    // ebt_free(ebt1);


    struct cqueue *cq = cqueue_new(1024 * 128, 512, 0);

    printCqueue(cq);

    struct item_bz bz = {"BB|||-----HELC%%%%", 1, &bz};
    char bf[256] = "%%||@$$$$___|||@#FEWCCCsabss%%";
    int a = 128;

    cqueue_unshift(cq, &bz, sizeof(struct item_bz));
    cqueue_unshift(cq, bf, 256);
    cqueue_unshift(cq, &a, sizeof(a));

    printCqueue(cq);

    struct item_bz az;
    int af[256];
    int b;

    cqueue_shift(cq, &az, sizeof(struct item_bz));
    err_msg("item_bz:[buf=%s] [a=%d] [next=%p]", az.buf, az.a, az.next);

    cqueue_shift(cq, af, 256);
    err_msg("af: [af=%s]", af);

    cqueue_shift(cq, &b, sizeof(b));
    err_msg("b: [b=%d]", b);

    printCqueue(cq);


    int s;
    for (s = 0; s < 10; s++)
    {
        struct item_bz ibz = {"bbbb||||-------------%%%%%%+++++++$$", rand() % 10, &ibz};
        cqueue_unshift(cq, &ibz, sizeof(struct item_bz));
    }

    printCqueue(cq);

    while (!cqueue_empty(cq))
    {
        struct item_bz tmp_bz;
        cqueue_shift(cq, &tmp_bz, sizeof(struct item_bz));
        err_msg("item_bz:[buf=%s] [a=%d] [next=%p]", tmp_bz.buf, tmp_bz.a, tmp_bz.next);
    }

    printCqueue(cq);

    cqueue_free(cq);

    printf("\n\n");

#if 0
    cq = cqueue_new(1024 * 80, 1000, QF_NOTIFY | QF_LOCK | QF_SHM);

    printCqueue(cq);

    pid_t pid;
    int z, worker_num = 5;
    for (z = 0; z < worker_num; z++)
    {
        if ((pid = fork()) < 0)
        {
            err_msg("fork");
            exit(1);
        }
        else if (pid > 0)
        {
            err_msg("created child process success. [pid = %d]", (int)pid);
            continue;
        }
        else
        {
            //child process
            int recvn = 0;
            struct item_bz tm_bz;
            char fname[56];
            char mbuff[256];
            FILE *fp;

            sprintf(fname, "/root/src/log_%d.txt", z);
            fp = fopen(fname, "a+");

            while (1)
            {
                if (cqueue_wait(cq) > 0)
                {
                    if (cqueue_pop(cq, &tm_bz, sizeof(struct item_bz)) < 0)
                        continue;
                    recvn++;

                    err_msg("worker[%d] recv: [buf=%s] [rand=%d]", z, tm_bz.buf, tm_bz.a);
                    sprintf(mbuff, "worker[%d] recv: [buf=%s] [rand=%d]\n", z, tm_bz.buf, tm_bz.a);

                    //write log to file[i];
                    fputs(mbuff, fp);
                }
            }
            err_msg("worker[%d] finish: [recvn=%d]", z, recvn);
            fclose(fp);
            exit(0);
        }
    }

    label: sleep(1);

    int sendn = 0;
    int inum = 10000;
    while (inum > 0)
    {
        struct item_bz ibz = {"--||||||||mmmmmm$$########", rand() % 11, &ibz};
        if (cqueue_push(cq, &ibz, sizeof (struct item_bz)) == 0)
        {
            cqueue_notify(cq);
            sendn++;
            inum--;
        }
    }

    printCqueue(cq);

    err_msg("master send finish: [num=%d] [sendn=%d]", inum, sendn);
    int status;
    for (z = 0; z < worker_num; z++)
    {
        wait(&status);
    }

    cqueue_free(cq);
#endif

#if 0 
    int iput;
    int num_tasks = 1000;
    int num_threads = 4;

    struct thread_pool pool;
    thread_pool_init(&pool, num_threads);

    thread_pool_run(&pool, thread_route);

    main_loop: sleep(2);
    //分发任务
    for (iput = 0; iput < num_tasks; iput++)
    {
        struct item_bz jobz = {"--|||||||||||nnnnnn%%%%%%;;;;;", rand() % 128, &jobz};
        thread_pool_dispatchq(&pool, &jobz, sizeof(struct item_bz));
    }

    thread_pool_free(&pool);
#endif

    struct ebt_srv srv; 
    ebt_srv_create(&srv);

    ebt_srv_on(&srv, E_CONNECT, on_connect);
    ebt_srv_on(&srv, E_RECEIVE, on_receive);
    ebt_srv_on(&srv, E_CLOSE, on_close);
    ebt_srv_on(&srv, E_SHUTDOWN, on_shutdown);

    ebt_srv_listen(&srv, 8080);
    ebt_srv_start(&srv);

    ebt_srv_free(&srv);
    return 0;   
}
