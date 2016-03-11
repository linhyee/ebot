###Libebt简介
Libebt是一个轻量级异步网络库, 可以用于构建小型高性能的网络应用服务。

###安装/使用

1.环境要求
Libebt目前只支持Linux环境上使用，而且它的异步事件操作用使了linux的epoll操作， 所以需要linux内核2.6以上。

2.安装
下载源码包，切换源码目录：`make`。

###异步IO
```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <ebt.h>

void main(void)
{
	struct timeval tv = {5, 0};
	char buf[128] = "hello, world!";

	void (*cb)(short, void*);
	void (*tcb)(short, void*);	

	struct eb_t ebt = ebt_new(E_READ | E_WRITE | E_TIMER);

	struct ev *ev_io = ev_read(0, cb, buf);
	struct ev *ev_t = ev_timer(&tv, tcb, buf);

	ev_attach(ebt, ev_io);
	ev_attach(ebt, ev_t);

	ebt_loop(ebt);

	ebt_free(ebt);
}

void cb(short num, void *arg)
{
	printf("num:%d, buf:%s\n", num, (char*)arg);
}

void tcb(short num, void *arg)
{
	printf("buf:%s\n", (char*)arg);
}
```

###线程池
```c
#include <ebt.h>
#include <time.h>

int main(void)
{
    int i;
    int num_tasks = 1000;
    int num_threads = 4;

    void *(*worker)(void*);

    struct thread_pool pool;
    thread_pool_init(&pool, num_threads);

    thread_pool_run(&pool, worker);

    main_loop: sleep(2);
    //分发任务
    for (i = 0; i < num_tasks; i++)
    {
        struct item_bz jobz = {"--|||||||||||nnnnnn%%%%%%;;;;;", rand() % 128, &jobz};
        thread_pool_dispatchq(&pool, &jobz, sizeof(struct item_bz));
    }

    thread_pool_free(&pool);

    return 0;
}
```