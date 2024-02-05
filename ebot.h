#ifndef __EBOT_H__
#define __EBOT_H__

#include <stdint.h>

#define STREAM_BUF_SIZE 128

typedef struct {
  int fd;
  size_t buf_size;
  char buf[STREAM_BUF_SIZE];
  union {
    void *ptr;
    uint32_t u32;
    uint64_t u64;
  } udata;
} eb_stream;

typedef enum {
  EB_CONNECT,
  EB_RECV,
  EB_WRITE,
  EB_CLOSE
} eb_event;

typedef struct {
  int backlog;
  int memory_pool_size;
  int reactor_num;
  int factory_num;
  int writer_num;
  int max_connections;
  int facotry_ringq_size;
  int timeout;
} eb_settings;

typedef struct ebot_srv ebot_srv;


extern ebot_srv* eb_srv_new(eb_settings *s);
extern void eb_srv_register_ptr(ebot_srv *srv, void *data);
extern void eb_srv_register_u32(ebot_srv *srv, uint32_t n);
extern void eb_srv_register_u64(ebot_srv *srv, uint64_t n);
extern int eb_srv_listen(ebot_srv *srv, size_t port);
extern void eb_srv_on(ebot_srv *srv, eb_event e, void (*f)(eb_stream *stream));
extern int eb_srv_start(ebot_srv *srv);
extern void eb_srv_destroy(ebot_srv *srv);

#endif