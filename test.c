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

  //test_ringq();
  //test_ringq_mulit();

  return 0;
}