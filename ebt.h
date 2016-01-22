#ifndef __EBT_H__
#define __EBT_H__


#define EBT_API extern

struct ev;
struct eb_t;

enum e_kind
{
	E_READ   = 0x01,	/* IO读 */
	E_WRITE  = 0x02,	/* IO写 */
	E_TIMER  = 0x04,	/* 定时器 */
	E_SIGNAL = 0x08,	/* 信号 */
	E_CHILD  = 0x10,	/* 进程 */
	E_FLAG   = 0x20		/* 用户自定义 */
};

enum e_opt
{
	E_ONCE = 0x01,	/* 当事件dipsatch到队列后,  从eb_t移除时, 标记为ONCE */
	E_FREE = 0x02	/* 事件已经从eb_t实例被移除, 将其标记为E_FREE, 释放其存储空间 */
};

typedef void e_cb_t(short, void *);

EBT_API struct ev * ev_read(int fd, e_cb_t *cb, void *arg, enum e_opt opt);
EBT_API struct ev * ev_write(int fd, e_cb_t *cb, void *arg, enum e_opt opt);
EBT_API struct ev * ev_timer(const struct timeval *tv, e_cb_t *cb, void *arg, enum e_opt opt);
EBT_API struct ev * ev_signal(int sig, e_cb_t *cb, void *arg, enum e_opt opt);
EBT_API struct ev * ev_child(pid_t child, e_cb_t *cb, void *arg, enum e_opt opt);
EBT_API struct ev * ev_flag(int flag, e_cb_t *cb, void *arg, enum e_opt opt);
EBT_API void ev_free(struct ev *e);

EBT_API int ev_attach(struct ev *e, struct eb_t *ebt);
EBT_API int ev_detach(struct ev *e);


EBT_API struct eb_t * ebt_new(enum e_kind kinds);
EBT_API int ebt_loop_once(struct eb_t *ebt);
EBT_API int ebt_loop(struct eb_t *ebt);
EBT_API void ebt_free(struct eb_t *ebt);

#endif