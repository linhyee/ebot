#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "ebot.h"

typedef struct {
  int fd;
  int tsz;
  char temp_buf[512];
} connnection;

#define MAX_CONN 1024

void on_connect(eb_stream *stream) {
  printf("fd=%d, client connected\n", stream->fd);
  connnection *conns = (connnection *)stream->udata.ptr;

  conns[stream->fd].fd = stream->fd;

  int i;
  for (i=0; i < MAX_CONN; i++) {
    if (conns[i].fd !=0 && conns[i].fd != stream->fd) {
      char buf[80];
      memset(buf, 0, sizeof(buf));
      snprintf(buf, sizeof(buf), "client %d connected", stream->fd);
      int ret = write(conns[i].fd, buf, strlen(buf) + 1);
      if (ret < 0) {
        puts("write error on connect");
      } else {
        printf("on_connect: write fd=%d, %s\n", conns[i].fd, buf);
      }
    }
  }
}

void on_close(eb_stream *stream) {
  printf("client %d closed\n", stream->fd);

  connnection *conns = (connnection *) stream->udata.ptr;
  int i;
  for (i=0; i < MAX_CONN; i++) {
    if (conns[i].fd != 0 && conns[i].fd != stream->fd) {
      char buf[80];
      memset(buf, 0, sizeof(buf));
      snprintf(buf, sizeof(buf), "client %d closed", stream->fd);
      int ret = write(conns[i].fd, buf, strlen(buf)+ 1);
      if (ret < 0) {
        puts("write error on close\n");
      } else {
        printf("on_close : write fd=%d, %s\n", conns[i].fd, buf);
      }
    }
  }
  memset(&conns[stream->fd], 0, sizeof(connnection));
}

void on_recv(eb_stream *stream) {
  connnection *conns = (connnection *) stream->udata.ptr;
  int i =0;
  for (i=0; i< MAX_CONN; i++) {
    if (conns[i].fd != 0 && conns[i].fd != stream->fd) {
      char buf[512];
      memset(buf, 0, sizeof(buf));

      snprintf(buf, 15, "client %d:", stream->fd);
      memcpy(buf + strlen(buf), stream->buf, stream->buf_size);

      int ret = write(conns[i].fd, buf, 15 + stream->buf_size);
      if (ret < 0) {
        puts("write error on recv\n");
      } else {
        printf("on_recv: write fd=%d, %s\n", conns[i].fd, stream->buf);
      }
    }
  }
}

int main(void) {

  connnection *conns = malloc(MAX_CONN * sizeof(connnection));
  assert(conns != NULL);
  memset(conns, 0, sizeof(MAX_CONN *sizeof(connnection)));

  ebot_srv *srv = eb_srv_new(NULL);

  eb_srv_register_ptr(srv, conns);

  eb_srv_listen(srv, 8080);

  eb_srv_on(srv, EB_CONNECT, on_connect);
  eb_srv_on(srv, EB_RECV, on_recv);
  eb_srv_on(srv, EB_CLOSE, on_close);

  eb_srv_start(srv);

  return 0;
}