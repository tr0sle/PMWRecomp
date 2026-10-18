/* ws2_32 emulation on a virtual LAN.
 *
 * Every player in an online room gets a virtual IPv4 address (10.77.0.N). UDP
 * datagrams, TCP streams and ICMP echo are carried as small packets over the
 * page's WebSocket to the room server (server/server.js), which routes them by
 * destination address and copies broadcasts to every room member. The game's
 * own LAN code (SBINET: broadcast discovery, TCP session, UDP traffic) then works
 * unchanged. Without a room, sockets fail exactly like an offline PC. */
#include "hle.h"
#include "js.h"
#include "win32.h"

int js_net_ip(void);                          /* our virtual address (in_addr as stored in memory), 0 = offline */
void js_net_send(const void *pkt, int len);   /* sends one packet to the room server */
int js_net_recv(void *buf, int max);          /* next received packet: length, 0 = none */
int js_net_peer_left(void);                   /* address of a member that left, 0 = none */
void js_net_name(char *buf, int max);         /* the player's lobby name */

/* winsock errors */
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAENOTSOCK 10038
#define WSAEMSGSIZE 10040
#define WSAEINVAL 10022
#define WSAEADDRINUSE 10048
#define WSAENETDOWN 10050
#define WSAECONNRESET 10054
#define WSAENOTCONN 10057
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAEISCONN 10056
#define WSAHOST_NOT_FOUND 11001

/* packet header (16 bytes, little-endian), followed by the payload */
enum { P_UDP = 1, P_SYN = 2, P_ACCEPT = 3, P_RST = 4, P_DATA = 5, P_FIN = 6, P_PING = 7, P_PONG = 8 };
typedef struct {
  uint8_t type, flags;
  uint16_t reserved;
  uint32_t src_ip, dst_ip;   /* src_ip is filled in by the server */
  uint16_t src_port, dst_port;
} PktHdr;
#define MAX_PKT 0x10000

enum { S_FREE, S_UDP, S_TCP, S_RAW };
enum { T_NEW, T_LISTEN, T_CONNECTING, T_CONNECTED, T_REFUSED, T_CLOSED };

typedef struct Dgram { struct Dgram *next; uint32_t ip; uint16_t port; int len; uint8_t data[]; } Dgram;

typedef struct {
  int type, state;
  int nonblocking, broadcast;
  uint16_t port;              /* local port, 0 = unbound */
  uint32_t peer_ip;           /* TCP peer / UDP connected peer */
  uint16_t peer_port;
  int err;                    /* pending SO_ERROR */
  int fin;                    /* peer closed its side */
  int accepted;               /* for sockets created by an incoming connection: returned by accept() yet? */
  int listener;               /* the listening socket that owns this pending connection */
  int backlog[64], nbacklog;  /* listening sockets: pending connection ids */
  uint8_t *rbuf;              /* TCP receive buffer */
  int rlen, rcap;
  Dgram *dq, *dq_tail;        /* UDP/RAW datagram queue */
  int dq_count;
} Sock;

#define MAX_SOCKS 128
#define SOCK_BASE 0x100
static Sock socks[MAX_SOCKS];
static uint32_t wsa_error;
static uint16_t next_port = 49152;

static int online(void) { return js_net_ip() != 0; }

static Sock *sock_get(uint32_t s) {
  uint32_t k = s - SOCK_BASE;
  if (k >= MAX_SOCKS || socks[k].type == S_FREE) return NULL;
  return &socks[k];
}
static int sock_id(Sock *s) { return (int)(s - socks) + SOCK_BASE; }

static uint16_t ephemeral_port(void) {
  for (;;) {
    uint16_t p = next_port++;
    if (next_port > 65000) next_port = 49152;
    int used = 0;
    for (int k = 0; k < MAX_SOCKS; k++)
      if (socks[k].type != S_FREE && socks[k].port == p) used = 1;
    if (!used) return p;
  }
}

static int is_broadcast(uint32_t ip) { return ip == 0xFFFFFFFFu || (ip >> 24) == 0xFF; }

static void send_pkt(int type, uint32_t dst_ip, uint16_t sport, uint16_t dport, const void *data, int len) {
  static uint8_t buf[sizeof(PktHdr) + MAX_PKT];
  PktHdr *h = (PktHdr *)buf;
  memset(h, 0, sizeof *h);
  h->type = (uint8_t)type;
  h->src_ip = (uint32_t)js_net_ip();
  h->dst_ip = dst_ip;
  h->src_port = sport;
  h->dst_port = dport;
  if (len > MAX_PKT) len = MAX_PKT;
  if (len > 0) memcpy(buf + sizeof *h, data, len);
  js_net_send(buf, (int)sizeof *h + (len > 0 ? len : 0));
}

static void dq_push(Sock *s, uint32_t ip, uint16_t port, const uint8_t *data, int len) {
  if (s->dq_count >= 256) return; /* full: drop like a real UDP stack */
  Dgram *d = malloc(sizeof *d + (len > 0 ? len : 0));
  d->next = NULL;
  d->ip = ip;
  d->port = port;
  d->len = len;
  if (len > 0) memcpy(d->data, data, len);
  if (s->dq_tail) s->dq_tail->next = d; else s->dq = d;
  s->dq_tail = d;
  s->dq_count++;
}

static Dgram *dq_pop(Sock *s) {
  Dgram *d = s->dq;
  if (!d) return NULL;
  s->dq = d->next;
  if (!s->dq) s->dq_tail = NULL;
  s->dq_count--;
  return d;
}

static void rbuf_append(Sock *s, const uint8_t *data, int len) {
  if (s->rlen + len > s->rcap) {
    s->rcap = (s->rlen + len) * 2 + 4096;
    s->rbuf = realloc(s->rbuf, s->rcap);
  }
  memcpy(s->rbuf + s->rlen, data, len);
  s->rlen += len;
}

static void sock_free(Sock *s) {
  Dgram *d;
  while ((d = dq_pop(s))) free(d);
  free(s->rbuf);
  memset(s, 0, sizeof *s);
}

static Sock *sock_new(int type) {
  for (int k = 0; k < MAX_SOCKS; k++)
    if (socks[k].type == S_FREE) {
      memset(&socks[k], 0, sizeof socks[k]);
      socks[k].type = type;
      socks[k].listener = -1;
      return &socks[k];
    }
  return NULL;
}

static Sock *find_tcp(uint16_t lport, uint32_t pip, uint16_t pport) {
  for (int k = 0; k < MAX_SOCKS; k++) {
    Sock *s = &socks[k];
    if (s->type == S_TCP && s->port == lport && s->peer_ip == pip && s->peer_port == pport &&
        (s->state == T_CONNECTING || s->state == T_CONNECTED))
      return s;
  }
  return NULL;
}

static uint16_t icmp_checksum(const uint8_t *p, int n) {
  uint32_t sum = 0;
  for (int i = 0; i + 1 < n; i += 2) sum += (uint32_t)p[i] | (uint32_t)p[i + 1] << 8;
  if (n & 1) sum += p[n - 1];
  sum = (sum >> 16) + (sum & 0xffff);
  sum += sum >> 16;
  return (uint16_t)~sum;
}

/* processes every packet that arrived from the room server */
static void net_pump(void) {
  static uint8_t buf[sizeof(PktHdr) + MAX_PKT];
  uint32_t gone;
  while ((gone = (uint32_t)js_net_peer_left()) != 0) {
    for (int k = 0; k < MAX_SOCKS; k++) {
      Sock *s = &socks[k];
      if (s->type == S_TCP && s->peer_ip == gone && (s->state == T_CONNECTED || s->state == T_CONNECTING)) {
        s->state = T_CLOSED;
        s->err = WSAECONNRESET;
      }
    }
  }
  int n;
  while ((n = js_net_recv(buf, sizeof buf)) > 0) {
    if (n < (int)sizeof(PktHdr)) continue;
    PktHdr *h = (PktHdr *)buf;
    uint8_t *data = buf + sizeof *h;
    int len = n - (int)sizeof *h;
    switch (h->type) {
      case P_UDP:
        for (int k = 0; k < MAX_SOCKS; k++) {
          Sock *s = &socks[k];
          if (s->type != S_UDP || s->port != h->dst_port) continue;
          dq_push(s, h->src_ip, h->src_port, data, len);
          break;
        }
        break;
      case P_SYN: {
        Sock *l = NULL;
        for (int k = 0; k < MAX_SOCKS; k++)
          if (socks[k].type == S_TCP && socks[k].state == T_LISTEN && socks[k].port == h->dst_port) l = &socks[k];
        if (!l || l->nbacklog >= 64) { send_pkt(P_RST, h->src_ip, h->dst_port, h->src_port, NULL, 0); break; }
        Sock *c = sock_new(S_TCP);
        if (!c) { send_pkt(P_RST, h->src_ip, h->dst_port, h->src_port, NULL, 0); break; }
        c->state = T_CONNECTED;
        c->port = h->dst_port;
        c->peer_ip = h->src_ip;
        c->peer_port = h->src_port;
        c->listener = sock_id(l);
        c->accepted = 0;
        l->backlog[l->nbacklog++] = sock_id(c);
        send_pkt(P_ACCEPT, h->src_ip, h->dst_port, h->src_port, NULL, 0);
        break;
      }
      case P_ACCEPT: {
        Sock *s = find_tcp(h->dst_port, h->src_ip, h->src_port);
        if (s && s->state == T_CONNECTING) s->state = T_CONNECTED;
        break;
      }
      case P_RST: {
        Sock *s = find_tcp(h->dst_port, h->src_ip, h->src_port);
        if (s) {
          s->err = s->state == T_CONNECTING ? WSAECONNREFUSED : WSAECONNRESET;
          s->state = s->state == T_CONNECTING ? T_REFUSED : T_CLOSED;
        }
        break;
      }
      case P_DATA: {
        Sock *s = find_tcp(h->dst_port, h->src_ip, h->src_port);
        if (s) rbuf_append(s, data, len);
        else send_pkt(P_RST, h->src_ip, h->dst_port, h->src_port, NULL, 0);
        break;
      }
      case P_FIN: {
        Sock *s = find_tcp(h->dst_port, h->src_ip, h->src_port);
        if (s) s->fin = 1;
        break;
      }
      case P_PING: /* echo request: answer with the same payload */
        send_pkt(P_PONG, h->src_ip, 0, 0, data, len);
        break;
      case P_PONG: {
        /* deliver an IPv4 + ICMP echo reply to every raw socket */
        if (len < 8 || len > 1400) break;
        uint8_t pkt[20 + 1400];
        memset(pkt, 0, 20);
        pkt[0] = 0x45;
        pkt[2] = (uint8_t)((20 + len) >> 8);
        pkt[3] = (uint8_t)(20 + len);
        pkt[8] = 128;
        pkt[9] = 1;
        memcpy(pkt + 12, &h->src_ip, 4);
        uint32_t me = (uint32_t)js_net_ip();
        memcpy(pkt + 16, &me, 4);
        memcpy(pkt + 20, data, len);
        pkt[20] = 0; /* echo reply */
        pkt[22] = pkt[23] = 0;
        uint16_t cs = icmp_checksum(pkt + 20, len);
        pkt[22] = (uint8_t)cs;
        pkt[23] = (uint8_t)(cs >> 8);
        for (int k = 0; k < MAX_SOCKS; k++)
          if (socks[k].type == S_RAW) dq_push(&socks[k], h->src_ip, 0, pkt, 20 + len);
        break;
      }
    }
  }
}

void net_poll(void) { if (online()) net_pump(); }

/* waits (letting the browser run) until cond() holds or timeout_ms passes */
static int wait_until(int (*cond)(Sock *), Sock *s, int timeout_ms) {
  double t0 = host_now_ms();
  for (;;) {
    net_pump();
    if (cond(s)) return 1;
    if (timeout_ms >= 0 && host_now_ms() - t0 > timeout_ms) return 0;
    js_sleep(2);
    note_yield();
  }
}

/* ------------------------------------------------------------------ API */
#define FAIL(n, e) do { wsa_error = (e); RET(n, 0xFFFFFFFFu); } while (0)
#define GET(n, sv) Sock *s = sock_get(sv); if (!s) FAIL(n, WSAENOTSOCK)

HLE(ws2_32, WSAStartup) { /* (version, wsadata*) */
  uint32_t p = ARG(1);
  memset(GPTR(p), 0, 400);
  ST16(p, 0x0202);
  ST16(p + 2, 0x0202);
  strcpy((char *)GPTR(p + 4), "WinSock 2.0");
  strcpy((char *)GPTR(p + 261), "Running");
  ST16(p + 390, 32767);
  RET(2, 0);
}
HLE(ws2_32, WSACleanup) { RET(0, 0); }
HLE(ws2_32, WSAGetLastError) { RET(0, wsa_error); }

HLE(ws2_32, socket) { /* (af, type, proto) */
  uint32_t af = ARG(0), type = ARG(1), proto = ARG(2);
  if (!online()) FAIL(3, WSAENETDOWN);
  int t = type == 1 ? S_TCP : type == 2 ? S_UDP : (type == 3 && proto == 1) ? S_RAW : 0;
  if (af != 2 || !t) FAIL(3, WSAEINVAL);
  Sock *s = sock_new(t);
  if (!s) FAIL(3, 10024);
  LOG("net: socket(type %u) -> %x\n", type, sock_id(s));
  RET(3, sock_id(s));
}

HLE(ws2_32, closesocket) {
  GET(1, ARG(0));
  if (s->type == S_TCP) {
    if (s->state == T_CONNECTED) send_pkt(P_FIN, s->peer_ip, s->port, s->peer_port, NULL, 0);
    if (s->state == T_LISTEN)
      for (int i = 0; i < s->nbacklog; i++) {
        Sock *c = sock_get(s->backlog[i]);
        if (c && !c->accepted) { send_pkt(P_RST, c->peer_ip, c->port, c->peer_port, NULL, 0); sock_free(c); }
      }
  }
  sock_free(s);
  RET(1, 0);
}

static uint32_t sa_ip(uint32_t sa) { return LD32(sa + 4); }
static uint16_t sa_port(uint32_t sa) { uint32_t v = LD16(sa + 2); return (uint16_t)((v >> 8) | ((v & 0xff) << 8)); }
static void sa_fill(uint32_t sa, uint32_t lenp, uint32_t ip, uint16_t port) {
  if (!sa) return;
  memset(GPTR(sa), 0, 16);
  ST16(sa, 2);
  ST16(sa + 2, (uint16_t)((port >> 8) | (port << 8)));
  ST32(sa + 4, ip);
  if (lenp) ST32(lenp, 16);
}

HLE(ws2_32, bind) { /* (s, addr, len) */
  GET(3, ARG(0));
  uint16_t port = sa_port(ARG(1));
  if (!port) port = ephemeral_port();
  else
    for (int k = 0; k < MAX_SOCKS; k++)
      if (&socks[k] != s && socks[k].type == s->type && socks[k].port == port && socks[k].state != T_CONNECTED)
        FAIL(3, WSAEADDRINUSE);
  s->port = port;
  LOG("net: bind(%x) port %u\n", ARG(0), port);
  RET(3, 0);
}

HLE(ws2_32, getsockname) { /* (s, addr, len*) */
  GET(3, ARG(0));
  if (!s->port && s->type != S_RAW) s->port = ephemeral_port();
  sa_fill(ARG(1), ARG(2), (uint32_t)js_net_ip(), s->port);
  RET(3, 0);
}

HLE(ws2_32, listen) {
  GET(2, ARG(0));
  if (s->type != S_TCP) FAIL(2, WSAEINVAL);
  if (!s->port) s->port = ephemeral_port();
  s->state = T_LISTEN;
  LOG("net: listen on %u\n", s->port);
  RET(2, 0);
}

static int has_backlog(Sock *s) { return s->nbacklog > 0; }

HLE(ws2_32, accept) { /* (s, addr, len*) */
  GET(3, ARG(0));
  if (s->state != T_LISTEN) FAIL(3, WSAEINVAL);
  net_pump();
  if (!s->nbacklog) {
    if (s->nonblocking) FAIL(3, WSAEWOULDBLOCK);
    wait_until(has_backlog, s, -1);
  }
  int id = s->backlog[0];
  memmove(s->backlog, s->backlog + 1, sizeof(int) * (--s->nbacklog));
  Sock *c = sock_get((uint32_t)id);
  if (!c) FAIL(3, WSAECONNRESET);
  c->accepted = 1;
  c->listener = -1;
  sa_fill(ARG(1), ARG(2), c->peer_ip, c->peer_port);
  LOG("net: accept on %u -> %x from port %u\n", s->port, id, c->peer_port);
  RET(3, id);
}

static int connect_done(Sock *s) { return s->state != T_CONNECTING; }

HLE(ws2_32, connect) { /* (s, addr, len) */
  GET(3, ARG(0));
  uint32_t ip = sa_ip(ARG(1));
  uint16_t port = sa_port(ARG(1));
  if (s->type == S_UDP) { s->peer_ip = ip; s->peer_port = port; if (!s->port) s->port = ephemeral_port(); RET(3, 0); }
  if (s->state == T_CONNECTED) FAIL(3, WSAEISCONN);
  if (s->state == T_CONNECTING) FAIL(3, 10037); /* WSAEALREADY */
  if (!s->port) s->port = ephemeral_port();
  s->peer_ip = ip;
  s->peer_port = port;
  s->state = T_CONNECTING;
  s->err = 0;
  LOG("net: connect %x -> %08x:%u\n", ARG(0), ip, port);
  send_pkt(P_SYN, ip, s->port, port, NULL, 0);
  if (s->nonblocking) FAIL(3, WSAEWOULDBLOCK);
  if (!wait_until(connect_done, s, 10000)) { s->state = T_REFUSED; FAIL(3, WSAETIMEDOUT); }
  if (s->state != T_CONNECTED) FAIL(3, WSAECONNREFUSED);
  RET(3, 0);
}

static int tcp_readable(Sock *s) { return s->rlen > 0 || s->fin || s->state == T_CLOSED || s->state == T_REFUSED; }

/* returns bytes read, or -1 with wsa_error set */
static int tcp_recv(Sock *s, uint32_t buf, int len) {
  net_pump();
  if (s->state != T_CONNECTED && !s->rlen) { wsa_error = s->err ? s->err : WSAENOTCONN; return -1; }
  if (!s->rlen && !s->fin) {
    if (s->nonblocking) { wsa_error = WSAEWOULDBLOCK; return -1; }
    wait_until(tcp_readable, s, -1);
    if (!s->rlen && !s->fin) { wsa_error = s->err ? s->err : WSAECONNRESET; return -1; }
  }
  int n = s->rlen < len ? s->rlen : len;
  memcpy(GPTR(buf), s->rbuf, n);
  memmove(s->rbuf, s->rbuf + n, s->rlen - n);
  s->rlen -= n;
  return n; /* 0 once the peer has closed and everything is read */
}

static int tcp_send(Sock *s, uint32_t buf, int len) {
  net_pump();
  if (s->state != T_CONNECTED) { wsa_error = s->err ? s->err : WSAENOTCONN; return -1; }
  for (int off = 0; off < len; off += 8192) {
    int n = len - off < 8192 ? len - off : 8192;
    send_pkt(P_DATA, s->peer_ip, s->port, s->peer_port, GPTR(buf + off), n);
  }
  return len;
}

HLE(ws2_32, recv) { /* (s, buf, len, flags) */
  GET(4, ARG(0));
  if (s->type != S_TCP) FAIL(4, WSAEINVAL);
  RET(4, (uint32_t)tcp_recv(s, ARG(1), (int)ARG(2)));
}

HLE(ws2_32, send) { /* (s, buf, len, flags) */
  GET(4, ARG(0));
  uint32_t buf = ARG(1);
  int len = (int)ARG(2);
  if (s->type == S_UDP) {
    if (!s->peer_ip) FAIL(4, WSAENOTCONN);
    send_pkt(P_UDP, s->peer_ip, s->port, s->peer_port, GPTR(buf), len);
    RET(4, len);
  }
  RET(4, (uint32_t)tcp_send(s, buf, len));
}

HLE(ws2_32, sendto) { /* (s, buf, len, flags, to, tolen) */
  GET(6, ARG(0));
  uint32_t buf = ARG(1), to = ARG(4);
  int len = (int)ARG(2);
  uint32_t ip = to ? sa_ip(to) : s->peer_ip;
  if (s->type == S_RAW) {
    send_pkt(P_PING, ip, 0, 0, GPTR(buf), len);
    RET(6, len);
  }
  if (s->type == S_TCP) RET(6, (uint32_t)tcp_send(s, buf, len));
  uint16_t port = to ? sa_port(to) : s->peer_port;
  if (!s->port) s->port = ephemeral_port();
  if (ip == 0x0100007Fu || ip == (uint32_t)js_net_ip()) {
    /* loopback: deliver locally */
    for (int k = 0; k < MAX_SOCKS; k++)
      if (socks[k].type == S_UDP && socks[k].port == port) { dq_push(&socks[k], (uint32_t)js_net_ip(), s->port, GPTR(buf), len); break; }
    RET(6, len);
  }
  send_pkt(P_UDP, ip, s->port, port, GPTR(buf), len);
  RET(6, len);
}

static int dq_ready(Sock *s) { return s->dq != NULL; }

HLE(ws2_32, recvfrom) { /* (s, buf, len, flags, from, fromlen*) */
  GET(6, ARG(0));
  uint32_t buf = ARG(1), from = ARG(4), fromlen = ARG(5);
  int len = (int)ARG(2);
  if (s->type == S_TCP) {
    sa_fill(from, fromlen, s->peer_ip, s->peer_port);
    RET(6, (uint32_t)tcp_recv(s, buf, len));
  }
  net_pump();
  if (!s->dq) {
    if (s->nonblocking) FAIL(6, WSAEWOULDBLOCK);
    wait_until(dq_ready, s, -1);
  }
  Dgram *d = dq_pop(s);
  sa_fill(from, fromlen, d->ip, d->port);
  int n = d->len < len ? d->len : len;
  memcpy(GPTR(buf), d->data, n);
  int trunc = d->len > len;
  free(d);
  if (trunc) FAIL(6, WSAEMSGSIZE);
  RET(6, n);
}

HLE(ws2_32, ioctlsocket) { /* (s, cmd, arg*) */
  GET(3, ARG(0));
  uint32_t cmd = ARG(1), argp = ARG(2);
  net_pump();
  if (cmd == 0x8004667Eu) { s->nonblocking = LD32(argp) != 0; RET(3, 0); } /* FIONBIO */
  if (cmd == 0x4004667Fu) {                                                /* FIONREAD */
    uint32_t n = s->type == S_TCP ? (uint32_t)s->rlen : s->dq ? (uint32_t)s->dq->len : 0;
    ST32(argp, n);
    RET(3, 0);
  }
  FAIL(3, WSAEINVAL);
}

HLE(ws2_32, setsockopt) { /* (s, level, name, val, len) */
  GET(5, ARG(0));
  if (ARG(1) == 0xffff && ARG(2) == 0x20) s->broadcast = LD32(ARG(3)) != 0;
  RET(5, 0);
}

HLE(ws2_32, getsockopt) { /* (s, level, name, val, len*) */
  GET(5, ARG(0));
  uint32_t val = ARG(3), lenp = ARG(4);
  net_pump();
  uint32_t v = 0;
  if (ARG(1) == 0xffff && ARG(2) == 0x1007) { v = (uint32_t)s->err; s->err = 0; } /* SO_ERROR */
  else if (ARG(1) == 0xffff && ARG(2) == 0x20) v = (uint32_t)s->broadcast;
  else if (ARG(1) == 0xffff && (ARG(2) == 0x1001 || ARG(2) == 0x1002)) v = 65536;
  else if (ARG(1) == 0xffff && ARG(2) == 0x1008) v = s->type == S_TCP ? 1 : 2; /* SO_TYPE */
  ST32(val, v);
  if (lenp) ST32(lenp, 4);
  RET(5, 0);
}

/* fd_set: u32 count, SOCKET array[64] */
static int sock_readable(Sock *s) {
  if (s->type == S_TCP) {
    if (s->state == T_LISTEN) return s->nbacklog > 0;
    return tcp_readable(s);
  }
  return s->dq != NULL;
}
static int sock_writable(Sock *s) {
  if (s->type == S_TCP) return s->state == T_CONNECTED || s->state == T_REFUSED || s->state == T_CLOSED;
  return 1;
}
static int sock_except(Sock *s) { return s->type == S_TCP && s->state == T_REFUSED; }

static int filter_set(uint32_t set, int (*pred)(Sock *)) {
  if (!set) return 0;
  uint32_t n = LD32(set), out = 0;
  if (n > 64) n = 64;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t sv = LD32(set + 4 + 4 * i);
    Sock *s = sock_get(sv);
    if (s && pred(s)) ST32(set + 4 + 4 * out++, sv);
  }
  ST32(set, out);
  return (int)out;
}
static int any_ready(uint32_t r, uint32_t w, uint32_t e) {
  int (*p[3])(Sock *) = {sock_readable, sock_writable, sock_except};
  uint32_t sets[3] = {r, w, e};
  for (int j = 0; j < 3; j++) {
    if (!sets[j]) continue;
    uint32_t n = LD32(sets[j]);
    for (uint32_t i = 0; i < n && i < 64; i++) {
      Sock *s = sock_get(LD32(sets[j] + 4 + 4 * i));
      if (s && p[j](s)) return 1;
    }
  }
  return 0;
}

HLE(ws2_32, select) { /* (nfds, read, write, except, timeout*) */
  uint32_t r = ARG(1), w = ARG(2), e = ARG(3), tv = ARG(4);
  net_pump();
  if (!any_ready(r, w, e)) {
    int timeout = tv ? (int)(LD32(tv) * 1000 + LD32(tv + 4) / 1000) : -1;
    double t0 = host_now_ms();
    while (timeout != 0) {
      js_sleep(2);
      note_yield();
      net_pump();
      if (any_ready(r, w, e)) break;
      if (timeout > 0 && host_now_ms() - t0 >= timeout) break;
    }
  }
  int n = filter_set(r, sock_readable) + filter_set(w, sock_writable) + filter_set(e, sock_except);
  RET(5, n);
}

HLE(ws2_32, __WSAFDIsSet) { /* (s, set) */
  uint32_t sv = ARG(0), set = ARG(1);
  uint32_t n = LD32(set);
  for (uint32_t i = 0; i < n && i < 64; i++)
    if (LD32(set + 4 + 4 * i) == sv) RET(2, 1);
  RET(2, 0);
}

/* ------------------------------------------------------------------ names */
static uint32_t hostent_mem;

static void host_name(char *out, int max) {
  js_net_name(out, max);
  if (!out[0]) snprintf(out, max, "player%u", (uint32_t)js_net_ip() >> 24);
}

HLE(ws2_32, gethostname) { /* (buf, len) */
  uint32_t buf = ARG(0), n = ARG(1);
  char name[64];
  host_name(name, sizeof name);
  if (n <= strlen(name)) FAIL(2, 10014);
  strcpy((char *)GPTR(buf), name);
  RET(2, 0);
}

static uint32_t make_hostent(const char *name, uint32_t ip) {
  if (!hostent_mem) hostent_mem = sys_alloc(256);
  uint32_t h = hostent_mem;
  memset(GPTR(h), 0, 256);
  /* hostent { char *h_name; char **h_aliases; short h_addrtype; short h_length; char **h_addr_list; } */
  uint32_t nm = h + 64, aliases = h + 32, list = h + 40, addr = h + 56;
  snprintf((char *)GPTR(nm), 128, "%s", name);
  ST32(h, nm);
  ST32(h + 4, aliases);
  ST16(h + 8, 2);
  ST16(h + 10, 4);
  ST32(h + 12, list);
  ST32(list, addr);
  ST32(list + 4, 0);
  ST32(aliases, 0);
  ST32(addr, ip);
  return h;
}

HLE(ws2_32, gethostbyname) {
  const char *name = ARGP(0);
  uint32_t ip = (uint32_t)js_net_ip();
  unsigned a, b, c, d;
  if (!ip) { wsa_error = WSAHOST_NOT_FOUND; RET(1, 0); }
  if (sscanf(name, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) RET(1, make_hostent(name, a | b << 8 | c << 16 | d << 24));
  char me[64];
  host_name(me, sizeof me);
  if (!strcmp(name, me)) RET(1, make_hostent(name, ip));
  if (!strcmp(name, "localhost")) RET(1, make_hostent(name, 0x0100007Fu));
  wsa_error = WSAHOST_NOT_FOUND;
  RET(1, 0);
}

HLE(ws2_32, inet_addr) {
  unsigned a, b, c, d;
  if (sscanf(ARGP(0), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) RET(1, a | b << 8 | c << 16 | d << 24);
  RET(1, 0xFFFFFFFFu);
}
HLE(ws2_32, htons) { uint32_t v = ARG(0) & 0xffff; RET(1, (v >> 8) | ((v & 0xff) << 8)); }
HLE(ws2_32, htonl) { RET(1, __builtin_bswap32(ARG(0))); }
