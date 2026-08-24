// htole16/le16toh and accept4 live behind feature-test macros; define the
// superset here so the build does not depend on the consumer's CMake
// extension settings
#define _GNU_SOURCE

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <l2cap.h>

// The kernel exposes no uapi headers for Bluetooth; these definitions are the
// stable socket ABI normally shipped by the BlueZ userspace headers.

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef BTPROTO_L2CAP
#define BTPROTO_L2CAP 0
#endif
#ifndef SOL_BLUETOOTH
#define SOL_BLUETOOTH 274
#endif
#ifndef BT_SECURITY
#define BT_SECURITY 4
#endif
#ifndef BT_SNDMTU
#define BT_SNDMTU 12
#endif
#ifndef BT_RCVMTU
#define BT_RCVMTU 13
#endif
#ifndef L2CAP_DEFAULT_MTU
#define L2CAP_DEFAULT_MTU 672
#endif

struct l2cap_sockaddr_l2 {
  sa_family_t l2_family;
  uint16_t l2_psm;
  uint8_t l2_bdaddr[6];
  uint16_t l2_cid;
  uint8_t l2_bdaddr_type;
};

// The kernel ABI this struct redeclares is frozen; catch any drift — size or
// field order — at compile time (C99 has no static_assert)
typedef char l2cap_sockaddr_l2_size_check[sizeof(struct l2cap_sockaddr_l2) == 14 ? 1 : -1];

typedef char l2cap_sockaddr_l2_layout_check
  [offsetof(struct l2cap_sockaddr_l2, l2_psm) == 2 &&
       offsetof(struct l2cap_sockaddr_l2, l2_bdaddr) == 4 &&
       offsetof(struct l2cap_sockaddr_l2, l2_cid) == 10 &&
       offsetof(struct l2cap_sockaddr_l2, l2_bdaddr_type) == 12
     ? 1
     : -1];

struct l2cap_bt_security {
  uint8_t level;
  uint8_t key_size;
};

struct l2cap_chunk_s {
  l2cap_chunk_t *next;
  size_t len;
  uint8_t data[];
};

enum {
  L2CAP_STATE_IDLE = 0,
  L2CAP_STATE_CONNECTING,
  L2CAP_STATE_OPEN,
  L2CAP_STATE_FAILED,
  L2CAP_STATE_CLOSED,
};

enum {
  L2CAP_SERVER_STATE_IDLE = 0,
  L2CAP_SERVER_STATE_LISTENING,
  L2CAP_SERVER_STATE_CLOSED,
};

static int
l2cap__hexval (char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int
l2cap_addr_init (const char *str, uint8_t type, l2cap_addr_t *addr) {
  if (strlen(str) != 17) return -EINVAL;

  // bdaddr_t is little-endian: byte 0 is the last octet of the string
  for (int i = 0; i < 6; i++) {
    const char *at = str + i * 3;

    int hi = l2cap__hexval(at[0]);
    int lo = l2cap__hexval(at[1]);
    if (hi < 0 || lo < 0) return -EINVAL;
    if (i < 5 && at[2] != ':') return -EINVAL;

    addr->bdaddr[5 - i] = (uint8_t) (hi << 4 | lo);
  }

  addr->type = type;

  return 0;
}

void
l2cap_addr_to_string (const l2cap_addr_t *addr, char *str) {
  const uint8_t *b = addr->bdaddr;
  snprintf(str, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[5], b[4], b[3], b[2], b[1], b[0]);
}

void
l2cap_channel_init (l2cap_channel_t *channel, void *data) {
  memset(channel, 0, sizeof(*channel));
  channel->data = data;
  channel->_fd = -1;
}

static int
l2cap__apply_security (int fd, uint8_t level) {
  if (level == 0) return 0; // not requested; kernel default applies

  struct l2cap_bt_security security;
  memset(&security, 0, sizeof(security));
  security.level = level;

  if (setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY, &security, sizeof(security)) != 0) return -errno;

  return 0;
}

int
l2cap_channel_set_security (l2cap_channel_t *channel, uint8_t level) {
  if (channel->_state != L2CAP_STATE_IDLE) return -EINVAL;
  if (level < L2CAP_SECURITY_LOW || level > L2CAP_SECURITY_FIPS) return -EINVAL;
  channel->_security = level;
  return 0;
}

static int
l2cap_channel__opened (l2cap_channel_t *channel) {
  uint16_t mtu = 0;
  socklen_t len = sizeof(mtu);
  if (getsockopt(channel->_fd, SOL_BLUETOOTH, BT_RCVMTU, &mtu, &len) != 0 || mtu == 0) {
    mtu = L2CAP_DEFAULT_MTU;
  }
  channel->_rcv_mtu = mtu;

  mtu = 0;
  len = sizeof(mtu);
  if (getsockopt(channel->_fd, SOL_BLUETOOTH, BT_SNDMTU, &mtu, &len) != 0 || mtu == 0) {
    mtu = L2CAP_DEFAULT_MTU;
  }
  channel->_snd_mtu = mtu;

  // _rcv_mtu is never 0 here: both branches above leave a positive value
  channel->_read_buf = malloc(channel->_rcv_mtu);
  if (channel->_read_buf == NULL) {
    channel->_state = L2CAP_STATE_FAILED;
    return -ENOMEM;
  }

  channel->_state = L2CAP_STATE_OPEN;

  return 0;
}

// A Bluetooth descriptor carries its endpoints; other families (e.g. the Unix
// socketpairs used in tests) leave them zeroed. The kernel fills the PSM on
// the peer name too, so one getpeername returns bdaddr, type and PSM at once —
// and unlike getsockname's sport, that PSM is correct for accepted channels.
static void
l2cap_channel__fill_endpoints (l2cap_channel_t *channel) {
  struct l2cap_sockaddr_l2 addr;
  socklen_t len = sizeof(addr);

  memset(&addr, 0, sizeof(addr));

  if (getpeername(channel->_fd, (struct sockaddr *) &addr, &len) != 0) return;
  if (addr.l2_family != AF_BLUETOOTH) return;

  memcpy(channel->_peer.bdaddr, addr.l2_bdaddr, sizeof(addr.l2_bdaddr));
  channel->_peer.type = addr.l2_bdaddr_type;
  channel->_psm = le16toh(addr.l2_psm);
}

int
l2cap_channel_connect (l2cap_channel_t *channel, const l2cap_addr_t *local, const l2cap_addr_t *peer, uint16_t psm, l2cap_connect_cb cb) {
  int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_L2CAP);
  if (fd < 0) return -errno;

  int sec_err = l2cap__apply_security(fd, channel->_security);
  if (sec_err < 0) {
    close(fd);
    return sec_err;
  }

  struct l2cap_sockaddr_l2 local_addr;
  memset(&local_addr, 0, sizeof(local_addr));
  local_addr.l2_family = AF_BLUETOOTH;
  local_addr.l2_bdaddr_type = local->type;
  memcpy(local_addr.l2_bdaddr, local->bdaddr, sizeof(local_addr.l2_bdaddr));

  if (bind(fd, (struct sockaddr *) &local_addr, sizeof(local_addr)) != 0) {
    int err = errno;
    close(fd);
    return -err;
  }

  struct l2cap_sockaddr_l2 peer_addr;
  memset(&peer_addr, 0, sizeof(peer_addr));
  peer_addr.l2_family = AF_BLUETOOTH;
  peer_addr.l2_psm = htole16(psm);
  peer_addr.l2_bdaddr_type = peer->type;
  memcpy(peer_addr.l2_bdaddr, peer->bdaddr, sizeof(peer_addr.l2_bdaddr));

  if (connect(fd, (struct sockaddr *) &peer_addr, sizeof(peer_addr)) != 0 && errno != EINPROGRESS) {
    int err = errno;
    close(fd);
    return -err;
  }

  channel->_fd = fd;
  channel->_state = L2CAP_STATE_CONNECTING;
  channel->_psm = psm;
  channel->_peer = *peer;
  channel->_on_connect = cb;

  return 0;
}

int
l2cap_channel_accept (l2cap_channel_t *channel, int fd) {
  if (channel->_state != L2CAP_STATE_IDLE) {
    close(fd); // ownership is taken in every case
    return -EINVAL;
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    int err = errno;
    close(fd);
    return -err;
  }

  fcntl(fd, F_SETFD, FD_CLOEXEC);

  channel->_fd = fd;

  l2cap_channel__fill_endpoints(channel);

  int err = l2cap_channel__opened(channel);
  if (err < 0) {
    close(fd);
    channel->_fd = -1;
    return err;
  }

  return 0;
}

int
l2cap_channel_fd (const l2cap_channel_t *channel) {
  return channel->_fd;
}

int
l2cap_channel_events (const l2cap_channel_t *channel) {
  switch (channel->_state) {
  case L2CAP_STATE_CONNECTING:
    return L2CAP_WRITABLE;

  case L2CAP_STATE_OPEN: {
    int events = 0;
    if (channel->_reading) events |= L2CAP_READABLE;
    if (channel->_write_head) events |= L2CAP_WRITABLE;
    return events;
  }

  default:
    return 0;
  }
}

static int
l2cap_channel__flush (l2cap_channel_t *channel) {
  while (channel->_write_head) {
    l2cap_chunk_t *chunk = channel->_write_head;
    if (send(channel->_fd, chunk->data, chunk->len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      if (errno == EINTR) continue;
      return -errno;
    }

    channel->_write_head = chunk->next;
    if (channel->_write_head == NULL) channel->_write_tail = NULL;
    free(chunk);
  }

  if (channel->_on_drain) {
    l2cap_drain_cb cb = channel->_on_drain;
    channel->_on_drain = NULL;
    cb(channel);
  }

  return 0;
}

// Processing order matters. A pending connect settles first, since nothing
// else is valid before it. Writes flush before reads so a full send queue
// drains as early as possible. Reads come last and every callback may close
// the channel, so each stage re-checks the state before touching the socket.
int
l2cap_channel_process (l2cap_channel_t *channel, int events) {
  if (channel->_state == L2CAP_STATE_CONNECTING && (events & L2CAP_WRITABLE)) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(channel->_fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;

    if (err == 0 && l2cap_channel__opened(channel) < 0) err = ENOMEM;

    if (err) {
      channel->_state = L2CAP_STATE_FAILED;
      channel->_on_connect(channel, -err);
      return 0;
    }

    channel->_on_connect(channel, 0);
  }

  if (channel->_state != L2CAP_STATE_OPEN) return 0;

  if (events & L2CAP_WRITABLE) {
    int err = l2cap_channel__flush(channel);
    if (err < 0) return err;
    if (channel->_state != L2CAP_STATE_OPEN) return 0;
  }

  if (events & L2CAP_READABLE) {
    while (channel->_reading && channel->_state == L2CAP_STATE_OPEN) {
      ssize_t n = recv(channel->_fd, channel->_read_buf, channel->_rcv_mtu, MSG_DONTWAIT);
      if (n > 0) {
        channel->_on_read(channel, (size_t) n, channel->_read_buf);
      } else if (n == 0) {
        channel->_on_read(channel, 0, NULL);
        break;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      } else if (errno == EINTR) {
        continue;
      } else {
        return -errno;
      }
    }
  }

  return 0;
}

int
l2cap_channel_read_start (l2cap_channel_t *channel, l2cap_read_cb cb) {
  if (channel->_state != L2CAP_STATE_OPEN) return -ENOTCONN;
  channel->_reading = 1;
  channel->_on_read = cb;
  return 0;
}

int
l2cap_channel_read_stop (l2cap_channel_t *channel) {
  channel->_reading = 0;
  return 0;
}

int
l2cap_channel_write (l2cap_channel_t *channel, const uint8_t *data, size_t len, l2cap_drain_cb cb) {
  if (channel->_state != L2CAP_STATE_OPEN) return -ENOTCONN;
  if (len == 0) return L2CAP_WRITE_SENT;
  if (len > channel->_snd_mtu) return -EMSGSIZE;

  if (channel->_write_head == NULL) {
    if (send(channel->_fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) return L2CAP_WRITE_SENT;
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -errno;
  }

  l2cap_chunk_t *chunk = malloc(sizeof(l2cap_chunk_t) + len);
  if (chunk == NULL) return -ENOMEM;

  chunk->next = NULL;
  chunk->len = len;
  memcpy(chunk->data, data, len);

  if (channel->_write_tail) channel->_write_tail->next = chunk;
  else channel->_write_head = chunk;
  channel->_write_tail = chunk;

  channel->_on_drain = cb;

  return L2CAP_WRITE_QUEUED;
}

uint16_t
l2cap_channel_psm (const l2cap_channel_t *channel) {
  return channel->_psm;
}

uint16_t
l2cap_channel_rcv_mtu (const l2cap_channel_t *channel) {
  return channel->_rcv_mtu;
}

uint16_t
l2cap_channel_snd_mtu (const l2cap_channel_t *channel) {
  return channel->_snd_mtu;
}

const l2cap_addr_t *
l2cap_channel_peer (const l2cap_channel_t *channel) {
  return &channel->_peer;
}

void
l2cap_channel_close (l2cap_channel_t *channel) {
  if (channel->_state == L2CAP_STATE_CLOSED) return;

  channel->_reading = 0;
  channel->_on_drain = NULL; // pending writes are discarded; no drain after close

  if (channel->_fd >= 0) close(channel->_fd);
  channel->_fd = -1;

  free(channel->_read_buf);
  channel->_read_buf = NULL;

  while (channel->_write_head) {
    l2cap_chunk_t *chunk = channel->_write_head;
    channel->_write_head = chunk->next;
    free(chunk);
  }
  channel->_write_tail = NULL;

  channel->_state = L2CAP_STATE_CLOSED;
}

static int
l2cap_server__set_nonblock_cloexec (int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -errno;

  fcntl(fd, F_SETFD, FD_CLOEXEC);

  return 0;
}

void
l2cap_server_init (l2cap_server_t *server, void *data) {
  memset(server, 0, sizeof(*server));
  server->data = data;
  server->_fd = -1;
}

int
l2cap_server_set_security (l2cap_server_t *server, uint8_t level) {
  if (server->_state != L2CAP_SERVER_STATE_IDLE) return -EINVAL;
  if (level < L2CAP_SECURITY_LOW || level > L2CAP_SECURITY_FIPS) return -EINVAL;
  server->_security = level;
  return 0;
}

int
l2cap_server_listen (l2cap_server_t *server, const l2cap_addr_t *local, uint16_t psm, int backlog) {
  int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_L2CAP);
  if (fd < 0) return -errno;

  int err = l2cap__apply_security(fd, server->_security);
  if (err < 0) {
    close(fd);
    return err;
  }

  struct l2cap_sockaddr_l2 local_addr;
  memset(&local_addr, 0, sizeof(local_addr));
  local_addr.l2_family = AF_BLUETOOTH;
  local_addr.l2_psm = htole16(psm);
  local_addr.l2_bdaddr_type = local->type;
  memcpy(local_addr.l2_bdaddr, local->bdaddr, sizeof(local_addr.l2_bdaddr));

  if (bind(fd, (struct sockaddr *) &local_addr, sizeof(local_addr)) != 0 || listen(fd, backlog) != 0) {
    int bind_err = errno;
    close(fd);
    return -bind_err;
  }

  // psm 0 lets the kernel assign one; read the assigned value back
  socklen_t len = sizeof(local_addr);
  if (getsockname(fd, (struct sockaddr *) &local_addr, &len) != 0) {
    int name_err = errno;
    close(fd);
    return -name_err;
  }

  server->_fd = fd;
  server->_state = L2CAP_SERVER_STATE_LISTENING;
  server->_psm = le16toh(local_addr.l2_psm);
  server->_local = *local;

  return 0;
}

int
l2cap_server_attach (l2cap_server_t *server, int fd) {
  if (server->_state != L2CAP_SERVER_STATE_IDLE) {
    close(fd); // ownership is taken in every case
    return -EINVAL;
  }

  int err = l2cap_server__set_nonblock_cloexec(fd);
  if (err < 0) {
    close(fd);
    return err;
  }

  server->_fd = fd;
  server->_state = L2CAP_SERVER_STATE_LISTENING;

  // A Bluetooth descriptor knows its endpoint; other families leave it zeroed
  struct l2cap_sockaddr_l2 addr;
  socklen_t len = sizeof(addr);
  memset(&addr, 0, sizeof(addr));
  if (getsockname(fd, (struct sockaddr *) &addr, &len) == 0 && addr.l2_family == AF_BLUETOOTH) {
    server->_psm = le16toh(addr.l2_psm);
    memcpy(server->_local.bdaddr, addr.l2_bdaddr, sizeof(addr.l2_bdaddr));
    server->_local.type = addr.l2_bdaddr_type;
  }

  return 0;
}

int
l2cap_server_fd (const l2cap_server_t *server) {
  return server->_fd;
}

int
l2cap_server_events (const l2cap_server_t *server) {
  if (server->_state != L2CAP_SERVER_STATE_LISTENING) return 0;
  return server->_accepting ? L2CAP_READABLE : 0;
}

int
l2cap_server_process (l2cap_server_t *server, int events) {
  if (server->_state != L2CAP_SERVER_STATE_LISTENING) return 0;
  if (server->_accepting && (events & L2CAP_READABLE)) server->_on_connection(server);
  return 0;
}

int
l2cap_server_accept_start (l2cap_server_t *server, l2cap_connection_cb cb) {
  if (server->_state != L2CAP_SERVER_STATE_LISTENING) return -EINVAL;
  server->_accepting = 1;
  server->_on_connection = cb;
  return 0;
}

int
l2cap_server_accept_stop (l2cap_server_t *server) {
  server->_accepting = 0;
  return 0;
}

int
l2cap_server_accept (l2cap_server_t *server, l2cap_channel_t *channel) {
  if (server->_state != L2CAP_SERVER_STATE_LISTENING) return -EINVAL;

  int fd = accept4(server->_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0) return -errno;

  return l2cap_channel_accept(channel, fd);
}

uint16_t
l2cap_server_psm (const l2cap_server_t *server) {
  return server->_psm;
}

const l2cap_addr_t *
l2cap_server_local (const l2cap_server_t *server) {
  return &server->_local;
}

void
l2cap_server_close (l2cap_server_t *server) {
  if (server->_state == L2CAP_SERVER_STATE_CLOSED) return;

  server->_accepting = 0;
  server->_on_connection = NULL;

  if (server->_fd >= 0) close(server->_fd);
  server->_fd = -1;

  server->_state = L2CAP_SERVER_STATE_CLOSED;
}
