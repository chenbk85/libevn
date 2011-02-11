
#include <errno.h> // errno
#include <unistd.h> // close
#include <stdlib.h> // free
#include <string.h> // memcpy

#include "evn.h"
#include "crossnet.h"

#define EVN_MAX_RECV 4096
static int evn_priv_unix_create(struct sockaddr_un* socket_un, char* sock_path);
static int evn_priv_tcp_create(struct sockaddr_in* socket_in, int port, char* sock_path);

static bool evn_stream_priv_send(struct evn_stream* stream, void* data, int size);
static char recv_data[EVN_MAX_RECV];

inline struct evn_stream* evn_stream_create(int fd) {
  evn_debug("evn_stream_create");
  struct evn_stream* stream;

  //stream = realloc(NULL, sizeof(struct evn_stream));
  stream = calloc(1, sizeof(struct evn_stream));
  stream->_priv_out_buffer = evn_inbuf_create(EVN_MAX_RECV);
  // stream->type = 0;
  // stream->oneshot = false;
  evn_set_nonblock(fd);
  ev_io_init(&stream->io, evn_stream_priv_on_read, fd, EV_READ);

  evn_debugs(".");
  return stream;
}

bool evn_stream_destroy(EV_P_ struct evn_stream* stream)
{
  bool result;

  // TODO delay freeing of server until streams have closed
  // or link loop directly to stream?
  result = close(stream->io.fd) ? true : false;
  ev_io_stop(EV_A_ &stream->io);
  stream->ready_state = evn_CLOSED;
  if (stream->on_close) { stream->on_close(EV_A_ stream, result); }
  evn_inbuf_destroy(stream->_priv_out_buffer);
  free(stream);

  return result;
}

// This callback is called when data is readable on the unix socket.
void evn_stream_priv_on_read(EV_P_ ev_io *w, int revents)
{
  void* data;
  struct evn_exception error;
  int length;
  struct evn_stream* stream = (struct evn_stream*) w;

  evn_debugs("EV_READ - stream->io.fd");
  length = recv(stream->io.fd, &recv_data, 4096, 0);

  if (length < 0)
  {
    if (stream->on_error) { stream->on_error(EV_A_ stream, &error); }
  }
  else if (0 == length)
  {
    evn_debugs("received FIN");
    // We no longer have data to ready but we may still have data to write
    stream->ready_state = evn_READ_ONLY;
    if (stream->io.events & EV_WRITE)
    {
      int fd = stream->io.fd;
      ev_io_stop(EV_A_ &stream->io);
      ev_io_set(&stream->io, fd, EV_WRITE);
      ev_io_start(EV_A_ &stream->io);
    }
    else
    {
      ev_io_stop(EV_A_ &stream->io);
    }
    if (stream->oneshot)
    {
      evn_debugs("oneshot shot");
      // TODO put on stack char data[stream->bufferlist->used];
      if (stream->bufferlist->used)
      {
        evn_buffer* buffer = evn_bufferlist_concat(stream->bufferlist);
        if (stream->on_data) { stream->on_data(EV_A_ stream, buffer->data, buffer->used); }
        free(buffer); // does not free buffer->data, that's up to the user
      }
    }
    if (stream->on_end) { stream->on_end(EV_A_ stream); }
    // evn_stream_destroy(EV_A_ stream);
  }
  else if (length > 0)
  {
    if (stream->oneshot)
    {
      evn_debugs("adding to buffer");
      // if (stream->on_progress) { stream->on_progress(EV_A_ stream, data, length); }
      // if time - stream->started_at > stream->max_wait; stream->timeout();
      // if buffer->used > stream->max_size; stream->timeout();
      evn_bufferlist_add(stream->bufferlist, &recv_data, length);
      return;
    }
    data = malloc(length);
    memcpy(data, &recv_data, length);
    if (stream->on_data) { stream->on_data(EV_A_ stream, recv_data, length); }
  }
}

void evn_server_priv_on_connection(EV_P_ ev_io *w, int revents)
{
  evn_debugs("new connection - EV_READ - server->io.fd has become readable");

  int stream_fd;
  struct evn_stream* stream;

  // since ev_io is the first member,
  // watcher `w` has the address of the
  // start of the evn_server struct
  struct evn_server* server = (struct evn_server*) w;

  while (1)
  {
    stream_fd = accept(server->io.fd, NULL, NULL);
    if (stream_fd == -1)
    {
      if( errno != EAGAIN && errno != EWOULDBLOCK )
      {
        fprintf(stderr, "accept() failed errno=%i (%s)", errno, strerror(errno));
        exit(EXIT_FAILURE);
      }
      break;
    }
    stream = evn_stream_create(stream_fd);
    stream->server = server;
    if (server->on_connection) { server->on_connection(EV_A_ server, stream); }
    if (stream->oneshot)
    {
      // each buffer chunk should be at least 4096 and we'll start with 128 chunks
      // the size will grow if the actual data received is larger
      stream->bufferlist = evn_bufferlist_create(4096, 128);
    }
    //stream->index = array_push(&server->streams, stream);
    ev_io_start(EV_A_ &stream->io);
  }
  evn_debugs(".");
}

// Simply adds O_NONBLOCK to the file descriptor of choice
int evn_set_nonblock(int fd)
{
  int flags;

  flags = fcntl(fd, F_GETFL);
  flags |= O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
}

static int evn_priv_tcp_create(struct sockaddr_in* socket_in, int port, char* address)
{
  int fd;

  // int optval = 1
  // setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))

  // Setup a tcp socket listener.
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == fd) {
    perror("[EVN] tcp socket create");
    exit(EXIT_FAILURE);
  }

  // Set it non-blocking
  if (-1 == evn_set_nonblock(fd)) {
    perror("echo server socket nonblock");
    exit(EXIT_FAILURE);
  }

  // Set it as a tcp socket
  socket_in->sin_family = AF_INET;
  socket_in->sin_addr.s_addr = inet_addr(address);
  socket_in->sin_port = htons(port);

  return fd;
}

static int evn_priv_unix_create(struct sockaddr_un* socket_un, char* sock_path)
{
  int fd;

  unlink(sock_path);

  // Setup a unix socket listener.
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (-1 == fd) {
    perror("echo server socket");
    exit(EXIT_FAILURE);
  }

  // Set it non-blocking
  if (-1 == evn_set_nonblock(fd)) {
    perror("echo server socket nonblock");
    exit(EXIT_FAILURE);
  }

  // Set it as unix socket
  socket_un->sun_family = AF_UNIX;
  strcpy(socket_un->sun_path, sock_path);
  evn_debug("sock_path: %s\n", sock_path);
  evn_debug("socket_un->sun_path: %s\n", socket_un->sun_path);

  return fd;
}

int evn_server_close(EV_P_ struct evn_server* server)
{
  ev_io_stop(server->EV_A_ &server->io);
  if (server->on_close) { server->on_close(server->EV_A_ server); }
  free(server);
  return 0;
}

int evn_server_destroy(EV_P_ struct evn_server* server)
{
  return evn_server_close(EV_A_ server);
}

struct evn_server* evn_server_create(EV_P_ evn_server_on_connection* on_connection)
{
  struct evn_server* server = calloc(1, sizeof(struct evn_server));
  server->EV_A = EV_A;
  server->on_connection = on_connection;
  return server;
}

int evn_server_listen(struct evn_server* server, int port, char* address)
{
  int max_queue = 1024;
  int fd;
  struct sockaddr_un* sock_unix;
  struct sockaddr_in* sock_inet;
  // TODO array_init(&server->streams, 128);

  if (0 == port)
  {
    server->socket = malloc(sizeof(struct sockaddr_un));
    sock_unix = (struct sockaddr_un*) server->socket;
    evn_debug("%s\n", address);
    fd = evn_priv_unix_create(sock_unix, address);
    server->socket_len = sizeof(sock_unix->sun_family) + strlen(sock_unix->sun_path) + 1;
  }
  else
  {
    server->socket = malloc(sizeof(struct sockaddr_in));
    sock_inet = (struct sockaddr_in*) server->socket;
    fd = evn_priv_tcp_create(sock_inet, port, address);
    server->socket_len = sizeof(struct sockaddr);
  }


  if (-1 == bind(fd, (struct sockaddr*) server->socket, server->socket_len))
  {
    perror("[EVN] server bind");
    exit(EXIT_FAILURE);
  }

  // TODO max_queue
  if (-1 == listen(fd, max_queue)) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  ev_io_init(&server->io, evn_server_priv_on_connection, fd, EV_READ);
  ev_io_start(server->EV_A_ &server->io);

  return 0;
}

bool evn_stream_end(EV_P_ struct evn_stream* stream)
{
  return evn_stream_destroy(EV_A_ stream);
}

bool evn_stream_write(EV_P_ struct evn_stream* stream, void* data, int size)
{
  if (0 == stream->_priv_out_buffer->size)
  {
    if (true == evn_stream_priv_send(stream, data, size))
    {
      evn_debugs("All data was sent without queueing");
      return true;
    }
    evn_debugs("Some data was queued");
    stream->_priv_out_buffer = evn_inbuf_create(size);
  }

  evn_debugs("ABQ data");

  evn_inbuf_add(stream->_priv_out_buffer, data, size);

  // Ensure that we listen for EV_WRITE
  if (!(stream->io.events & EV_WRITE))
  {
    int fd = stream->io.fd;
    ev_io_stop(EV_A_ &stream->io);
    ev_io_set(&stream->io, fd, EV_READ | EV_WRITE);
  }
  ev_io_start(EV_A_ &stream->io);

  return false;
}

static bool evn_stream_priv_send(struct evn_stream* stream, void* data, int size)
{
  //const int MAX_SEND = 4096;
  int sent;
  evn_inbuf* buf = stream->_priv_out_buffer;
  int buf_size = buf->size;

  evn_debugs("priv_send");
  if (0 != buf_size)
  {
    evn_debugs("has buffer with data");
    sent = send(stream->io.fd, buf->bottom, buf->size, MSG_DONTWAIT | EVN_NOSIGNAL);
    if (sent < 0)
    {
      perror("send failed");
      return false;
    }
    evn_inbuf_toss(buf, sent);

    if (sent != buf_size)
    {
      evn_debugs("buffer wasn't emptied");
      if (NULL != data && 0 != size) { evn_inbuf_add(buf, data, size); }
      return false;
    }

    if (NULL != data && 0 != size)
    {
      evn_debugs("and has more data");
      sent = send(stream->io.fd, data, size, MSG_DONTWAIT | EVN_NOSIGNAL);
      if (sent < 0)
      {
        perror("send failed");
        return false;
      }
      if (sent != size) {
        evn_debugs("enqueued remaining data");
        evn_inbuf_add(buf, data + sent, size - sent);
        return false;
      }
    }
    return true;
  }

  if (NULL != data && 0 != size)
  {
    evn_debugs("doesn't have data in buffer, but does have data");
    sent = send(stream->io.fd, data, size, MSG_DONTWAIT | EVN_NOSIGNAL);
    if (sent < 0)
    {
      perror("send failed");
      return false;
    }
    if (sent != size) {
      evn_debugs("could not send all of the data");
      evn_inbuf_add(buf, data + sent, size - sent);
      return false;
    }
    evn_debugs("sent all of the data");
  }

  return true;
}

static void evn_stream_priv_on_writable(EV_P_ ev_io *w, int revents)
{
  struct evn_stream* stream = (struct evn_stream*) w;

  evn_debugs("EV_WRITE");

  evn_stream_priv_send(stream, NULL, 0);

  // If the buffer is finally empty, send the `drain` event
  if (0 == stream->_priv_out_buffer->size)
  {
    int fd = stream->io.fd;
    ev_io_stop(EV_A_ &stream->io);
    ev_io_set(&stream->io, fd, EV_READ);
    ev_io_start(EV_A_ &stream->io);
    evn_debugs("pre-drain");
    if (stream->on_drain) { stream->on_drain(EV_A_ stream); }
    // and the 
    return;
  }
  evn_debugs("post-null");
}

static inline void evn_stream_priv_on_activity(EV_P_ ev_io *w, int revents)
{
  evn_debugs("Stream Activity");
  if (revents & EV_READ)
  {
    // evn_stream_read_priv_cb
  }
  else if (revents & EV_WRITE)
  {
    evn_stream_priv_on_writable(EV_A, w, revents);
  }
  else
  {
    // Never Happens
    evn_debugs("[ERR] ev_io received something other than EV_READ or EV_WRITE");
  }
}

static void evn_stream_priv_on_connect(EV_P_ ev_io *w, int revents)
{
  struct evn_stream* stream = (struct evn_stream*) w;
  evn_debugs("Stream Connect");

  //ev_cb_set (ev_TYPE *watcher, callback)
  //ev_io_set (&w, STDIN_FILENO, EV_READ);

  int fd = stream->io.fd;
  ev_io_stop(EV_A_ &stream->io);
  ev_io_init(&stream->io, evn_stream_priv_on_activity, fd, EV_WRITE);
  ev_io_start(EV_A_ &stream->io);
  //ev_cb_set(&stream->io, evn_stream_priv_on_activity);

  if (stream->on_connect) { stream->on_connect(EV_A_ stream); }
}

inline struct evn_stream* evn_create_connection(EV_P_ int port, char* address)
{
  if (0 == port )
  {
    return evn_create_connection_unix_stream(EV_A_ address);
  }
  return evn_create_connection_tcp_stream(EV_A_ port, address);
}

struct evn_stream* evn_create_connection_tcp_stream(EV_P_ int port, char* address)
{
  int stream_fd;
  struct evn_stream* stream;
  struct sockaddr_in* socket_in;

  stream_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == stream_fd) {
    perror("[EVN] TCP socket connection");
    exit(EXIT_FAILURE);
  }
  stream = evn_stream_create(stream_fd);
  stream->socket = malloc(sizeof(struct sockaddr_in));
  socket_in = (struct sockaddr_in*) stream->socket;

  socket_in->sin_family = AF_INET;
  socket_in->sin_addr.s_addr = inet_addr(address);
  socket_in->sin_port = htons(port);

  stream->socket_len = sizeof(struct sockaddr);

  if (-1 == connect(stream_fd, (struct sockaddr*) stream->socket, stream->socket_len)) {
      perror("connect");
      exit(EXIT_FAILURE);
  }
  
  return stream;
}

struct evn_stream* evn_create_connection_unix_stream(EV_P_ char* sock_path)
{
  int stream_fd;
  struct evn_stream* stream;
  struct sockaddr_un* sock;

  stream_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (-1 == stream_fd) {
      perror("[EVN] Unix Stream socket connection");
      exit(EXIT_FAILURE);
  }
  stream = evn_stream_create(stream_fd);
  stream->socket = malloc(sizeof(struct sockaddr_un));
  sock = (struct sockaddr_un*) stream->socket;

  // initialize the connect callback so that it starts the stdin asap
  ev_io_init(&stream->io, evn_stream_priv_on_connect, stream_fd, EV_WRITE);
  ev_io_start(EV_A_ &stream->io);

  sock->sun_family = AF_UNIX;
  strcpy(sock->sun_path, sock_path);
  stream->socket_len = strlen(sock->sun_path) + 1 + sizeof(sock->sun_family);

  if (-1 == connect(stream_fd, (struct sockaddr *) sock, stream->socket_len)) {
      perror("connect");
      exit(EXIT_FAILURE);
  }
  
  return stream;
}
