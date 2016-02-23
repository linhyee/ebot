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
#include <sys/mman.h>

/******************************************************************/
/* error functions                                                */
/******************************************************************/
#define ERR_MAXLINE	2048

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
  * \param errnoflag int
  * \param fmt       const char*
  * \param ap        va_list
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
#ifdef DEBUG
#define err_debug(fmt, ...) err_msg(fmt, ## __VA_ARGS__)
#else
#define err_debug(fmt, ...)
#endif

/******************************************************************/
/* chan                                                           */
/******************************************************************/
enum chan_flag
{
	CHAIN_LOCK   = 1u << 1,
	CHAIN_NOTIFY = 1u << 2,
	CHAIN_SHM    = 1u << 3,
};

struct chan_t
{
	int length;
	char data[0];
};

struct chan
{
	int head;					/* queue head */
	int tail; 					/* queue tail */
	int size; 					/* the queue capacity */
	char head_tag;				/* tag whether elem already in head */
	char tail_tag;				/* tag whether elem already in tail*/
	int num; 					/* current total elements */
	int flag;					/* queue flag */
	int maxlen;					/* max element size */
	void *mem;					/* memory block */
	pthread_mutex_t lock;
	pthread_mutexattr_t attr;
	int eventfd; 				/* <sys/eventfd.h> for notification */
};

#define CHAIN_MINMEM_LENGTH (1024 * 64) //最小内存分配
#define chan_empty(q)		(q->num == 0)
#define chan_full(q)		((q->head == q->tail) && ( q->tail_tag != q->head_tag))

struct chan * chan_new(int size, int maxlen, int flag)
{
	assert(size > CHAIN_MINMEM_LENGTH + maxlen);
	void *mem;
	int  ret, efd;

	if (flag & CHAIN_SHM)
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

		mem = mmap(NULL, size, PROT_READ | PROT_WRITE, shmflag, shmfd, 0);

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
		mem = malloc(size);

		if (mem == NULL)
		{
			err_msg ("malloc fail!");
			return NULL;
		}
	}

	struct chan *ch = mem;
	mem += sizeof(struct chan);
	memset(ch, 0, sizeof(struct chan));

	ch->mem    = mem;
	ch->size   = size;
	ch->maxlen = maxlen;
	ch->flag   = flag;

	if (flag & CHAIN_LOCK)
	{
		pthread_mutexattr_init(&ch->attr);
		pthread_mutexattr_setpshared(&ch->attr, PTHREAD_PROCESS_SHARED);

		ret = pthread_mutex_init(&ch->lock, &ch->attr);

		if (ret < 0)
		{
			if (flag & CHAIN_SHM)
				munmap(mem, size);
			else
				free(mem);

			err_msg ("mutext init failed!");

			return NULL;
		}
	}

	if (flag & CHAIN_NOTIFY)
	{
		efd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
		ch->eventfd = efd;
	}

	return ch;
}

int chan_pop(struct chan *ch, void *out, int buf_len)
{
	assert(ch->flag & CHAIN_LOCK);

	int n;
	pthread_mutex_lock(&ch->lock);
	n = chan_out(ch, out, buf_len);
	pthread_mutex_unlock(&ch->lock);

	return n;
}

int chan_push(struct chan *ch, void *in, int buf_len)
{
	assert(ch->flag & CHAIN_LOCK);

	int ret;
	pthread_mutex_lock(&ch->lock);
	ret = chan_in(ch, in, buf_len);
	pthread_mutex_unlock(&ch->lock);

	return ret;
}

int chan_out(struct chan *ch, void *out, int buf_len)
{
	//队列为空
	if (chan_empty(ch))	
	{
		//这里非常重要,避免此线程再次获得锁
		sched_yield(); // or usleep(1);
		return -1;
	}
	struct chan_t *cht = ch->mem + ch->head;
	assert(buf_len >= cht->length);

	memcpy(out, cht->data, cht->length);
	ch->head += (cht->length + sizeof(cht->length));

	if (ch->head >= ch->size)
	{
		ch->head = 0;
		ch->head_tag = 1 - ch->head_tag;
	}

	ch->num--;

	return cht->length;
}

int chan_in(struct chan *ch, void *in, int buf_len)
{
	assert(buf_len < ch->maxlen);

	//队列满
	if (chan_full(ch))
	{
		sched_yield();
		return -1;
	}

	struct chan_t *cht;
	int msize;

	msize = sizeof(cht->length) + buf_len;

	if (ch->tail < ch->head)
	{
		if ((ch->head - ch->tail) < msize)
			return -1;

		cht = ch->mem + ch->tail;
		ch->tail += msize;
	}
	else
	{
		cht = ch->mem + ch->tail;
		ch->tail += msize;
		if (ch->tail >= ch->size)
		{
			ch->tail = 0;
			ch->tail_tag = 1 - ch->tail_tag;
		}
	}
	ch->num++;
	cht->length = buf_len;
	memcpy(cht->data, in, buf_len);

	return 0;
}

int chan_wait(struct chan *ch)
{
	assert(ch->flag & CHAIN_NOTIFY);

	int ret, timeout = 0;
	uint64_t flag;

	while (1)
	{
		ret = read(ch->eventfd, &flag, sizeof(uint64_t));

		if (ret < 0 && errno == EINTR)
			continue;
		break;
	}

	return 0;
}

int chan_notify(struct chan *ch)
{
	assert(ch->flag & CHAIN_NOTIFY);
	int ret;
	uint64_t flag = 1;	
	
	while (1)	
	{
		ret = write(ch->eventfd, &flag, sizeof(uint64_t));

		if (ret < 0 && errno == EINTR)
				continue;
		break;
	}

	return ret;
}

void chan_free(struct chan *ch)
{
	if (ch->flag & CHAIN_LOCK)
		pthread_mutex_destroy(&ch->lock);

	if (ch->flag & CHAIN_NOTIFY)
		close(ch->eventfd);

	if (ch->flag & CHAIN_SHM)
		munmap(ch->mem, ch->size);
	else
	{
		if (ch->mem)
			free(ch->mem);
	}
}

/******************************************************************/
/* thread pool                                                    */
/******************************************************************/
/*struct thd_pool;
struct thd_param
{
	void *data;
	int thd_id;
};

struct thd_entity
{
	pthread_t tid;
	int id;
	struct thd_pool *pool;
};

struct thd_pool
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct thd_entity *threads;
	struct thd_param *params;

	struct chan *ch;

	int thread_num;
	int shutdown;
	volatile uint32_t task_num;
};

int thd_pool_create(struct thd_pool *pool, int num)
{
	memset(pool, 0, sizeof(struct thd_pool));
	pool->threads = malloc(num * sizeof(struct thd_pool));
	if (pool->threads == NULL)
	{
		free(pool->threads);
		err_msg("malloc failed!");
		return -1;
	}

	pool->params  = malloc(num * sizeof(struct thd_param));

	if (pool->params == NULL)
	{
		free(pool->params);
		err_msg("malloc failed!");
		return -1;
	}

	//create chan
	pool->ch = chan_new(1024 * 256, 512, 0);

	if (pool->ch == NULL)
	{
		free(pool->threads);
		free(pool->params);
		err_msg("malloc failed!");
		return -1;
	}

	pthread_mutex_init(&pool->mutex, NULL);
	pthread_cond_init(&pool->cond, NULL);
	pool->thread_num = num;

	return 0;
}

//分发任务
int thd_pool_dispatch(struct thd_pool *pool, void *task, int task_len)
{
	int i, ret;
	pthread_mutex_lock(&pool->mutex);

	//try 1000 times
	for (i = 0; i < 1000; i++)
	{
		ret = chan_in(pool->ch, task, int task_len);

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

	volatile uint32_t *task_num = &pool->task_num;
	__sync_fetch_and_add(task_num, 1);

	return pthread_cond_signal(&pool->cond);
}

int thd_pool_run(struct thd_pool *pool)
{
	int i, ret;
	for (i = 0; i < pool->thread_num; i++)
	{
		pool->params[i].thd_id = i;
		pool->params[i].data = pool;
		ret = pthread_create(& ((&pool->threads[i])->tid), NULL, thd_pool_loop, &pool->params[i]);

		if (ret < 0)
		{
			err_msg("pthread_create failed, error for %s!", strerror(errno));
			return -1;
		}
	}
	return 0;
}

int thd_pool_free(struct thd_pool *pool)
{
	if (pool->shutdown)
		return -1;
	pthread_cond_broadcast(&pool->cond);

	int i;
	for (i = 0; i < pool->thread_num; i++)
		pthread_join(& ((&pool->threads[i])->tid), NULL)

	chan_free(pool->chan);
	pthread_mutex_destroy(&pool->mutex);
	pthread_cond_destroy(&pool->cond);

	return 0;
}

static void* thd_pool_loop(void *arg)
{
	struct thd_param *param = arg;
	struct thd_pool *pool = param->data;
	int ret, runnig, id = param->thd_id;
	void *task;

	while(runnig)
	{
		pthread_mutex_lock(&pool->mutex);

		if (pool->shutdown)
		{
			pthread_mutex_unlock(&pool->mutex);
			err_msg("thread [%d] will exit", id);
			pthread_exit(NULL);
		}

		if (pool->task_num == 0)
			pthread_cond_wait(&pool->cond, &pool->mutex);

		err_msg("thread [%d] is starting to work", id);

		ret = chan_out(&pool->ch, task, BUFSIZE);
		pthread_mutex_unlock(&pool->mutex);

		if (ret >= 0)
		{
			volatile uint32_t *task_num = &pool->task_num;
			__sync_fetch_and_sub(task_num, 1);

			pool->task(pool, (void *)task, ret);
		}
	}

	if (pool->stop)
		pool->stop(pool, id);

	pthread_exit(NULL);
	return NULL;
}*/

/******************************************************************/
/* Reactor                                                        */
/******************************************************************/
typedef void e_cb_t(short events, void *arg);
struct eb_t;
struct eb_o;

enum e_opt
{
	E_ONCE = 0x01,	/* 一次性事件, 当事件dipsatch到队列后,  激活后立该从eb_t移除, 并标记为ONCE */
	E_FREE = 0x02	/* 事件已经从eb_t实例被移除, 将其标记为E_FREE, 以便释放其存储空间 */
};

enum
{
	E_QUEUE = 0x80 //标记是否已经在dispatchq队列
};

enum e_kide
{
	E_READ   = 0x01,	/* IO读 */
	E_WRITE  = 0x02,	/* IO写 */
	E_TIMER  = 0x04,	/* 定时器 */
	E_SIGNAL = 0x08,	/* 信号 */
	E_CHILD  = 0x10,	/* 进程 */
	E_FLAG   = 0x20		/* 用户自定义 */
};

struct ev
{
	enum e_kide kide;			/* 事件类型 */
	enum e_opt opt;				/* 事件的标记 */
	e_cb_t *cb;					/* 事件回调函数 */
	struct eb_t *ebt;			/* 指向eb_t结构体的实例 */
	void *arg;					/* 事件参数 */

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
	const struct eb_o *ebo;			/* 操作eb_t实例的对象 */
	enum e_kide kides; 				/* 所允许的支持事件类型 */
	unsigned int num;				/* 注册到eb_t实例的事件总数 */
	unsigned int numtimers;			/* 当前定时器的总数 */
	unsigned int maxtimers;			/* 最大定时器数 */
	struct timeval timerdebt;		/* 用于定时器相减 */
	int broken;						/* 中断调用 */

	TAILQ_HEAD(, ev) dispatchq;		/* 事件就绪队列 */
	TAILQ_HEAD(, ev_flag) flags;	/* 自定义事件队列 */
	RB_HEAD(timer_tree, ev_timer) timers; /* 定时器队列 */
};

/**
 * ebt oprerations 反应堆的实例操作元
 */
struct eb_o
{
	const char 		*name;
	enum e_kide 	kides;
	size_t 			ebtsz;	/* 从eb_t派生的结构体的大小 */

	int (*construct)(struct eb_t *);
	int (*destruct) (struct eb_t *);
	int (*init)		(struct eb_t *);
	int (*loop)		(struct eb_t *, const struct timeval *);
	int (*attach)	(struct eb_t *, struct ev *);	
	int (*detach)	(struct eb_t *, struct ev *);
	int (*free)	(struct eb_t *);
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
#define EP_SIZE	32000

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do {      \
	if (fcntl(x, F_SETFD, 1) == -1)	\
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

static int epoll_init		(struct eb_t *);
static int epoll_loop		(struct eb_t *, const struct timeval *);
static int epoll_attach		(struct eb_t *, struct ev *);
static int epoll_detach		(struct eb_t *, struct ev *);
static int epoll_free		(struct eb_t *);

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
	struct epoll_event 	ev;
	struct ebt_epoll 	*epo     = (struct ebt_epoll *) ebt;
	struct ev_io 		*evf     = (struct ev_io *) e;

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
	struct epoll_event 	ev;
	struct ebt_epoll 	*epo = (struct ebt_epoll *) ebt;
	struct ev_io 		*evf = (struct ev_io *) e;

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
/* test                                                           */
/******************************************************************/
struct ev_param 
{
	char buf[256];
	struct timeval tv;
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



	// test event loop
	struct eb_t *ebt1 = ebt_new(E_READ | E_WRITE | E_TIMER);

	for (j = 0; j < 10; j++)
	{
		struct ev_param *dt = calloc(1, sizeof(struct ev_param));
		memcpy(dt->buf, buf, strlen(buf) + 1);
		tv.tv_sec     = rand() % 15 + 5;
		tv.tv_usec    = 0;
		dt->tv        = tv;

		struct ev *et = ev_timer(&tv, tcb, dt);
		ret           = ev_attach(et, ebt1);
	}

	printEbt(ebt1);

	ebt_loop(ebt1);

	printEbt(ebt1);

	ebt_free(ebt1);

	return 0;	
}
