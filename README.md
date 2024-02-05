### Libebt简介
ebot是一个轻量级异步网络库, 可以用于构建小型高性能的网络应用服务。

### 安装/使用

1.环境要求

Libebt目前只支持Linux环境上使用，而且它的异步事件操作用使了linux的epoll接口， 所以需要linux内核2.6以上。

2.安装

下载源码包，切换源码目录：`make`。

### 异步TCP服务
```c
#include <stdlib.h>
#include <stdio.h>
#include "ebot.h"

void on_connect(eb_stream *stream) {
  printf("fd=%d, client connected\n", stream->fd);
}

void on_recv(eb_stream *stream) {
  printf("fd=%d, recv client buf=%s\n", stream->fd, stream->buf);
}

void on_write(eb_stream *stream) {
  //printf("fd=%d, write can write");
}

void on_close(eb_stream *stream) {
  printf("fd=%d, client closed\n", stream->fd);
}

int main(void) {
  ebot_srv *srv = eb_srv_new(NULL);

  eb_srv_listen(srv, 10086);

  eb_srv_on(srv, EB_CONNECT, on_connect);
  eb_srv_on(srv, EB_RECV, on_recv);
  eb_srv_on(srv, EB_WRITE, on_write);
  eb_srv_on(srv, EB_CLOSE, on_close);

  eb_srv_start(srv);

  return 0;
}
```
