/* Fast unlimited-rate MQTT 3.1.1 QoS1 publisher used by
 * multi_pub_qos1_integration.py. No broker/runtime dependency. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct pub {
  int fd;
  int window;
  volatile int inflight;
  volatile int stop;
  volatile unsigned long acked;
  pthread_t ack_th;
};

static int write_all(int fd, const void *buf, size_t n) {
  const unsigned char *p = buf;
  while (n) {
    ssize_t k = write(fd, p, n);
    if (k < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (k == 0)
      return -1;
    p += (size_t)k;
    n -= (size_t)k;
  }
  return 0;
}

static int encode_remaining(unsigned char *out, size_t size) {
  int n = 0;
  do {
    unsigned char byte = (unsigned char)(size % 128);
    size /= 128;
    if (size)
      byte |= 0x80;
    out[n++] = byte;
  } while (size && n < 4);
  return n;
}

static int mqtt_connect(int fd, const char *client_id) {
  unsigned char body[128];
  size_t off = 0;
  size_t id_len = strlen(client_id);
  /* protocol name MQTT, level 4, flags=clean, keepalive 60 */
  body[off++] = 0;
  body[off++] = 4;
  memcpy(body + off, "MQTT", 4);
  off += 4;
  body[off++] = 4;
  body[off++] = 0x02;
  body[off++] = 0;
  body[off++] = 60;
  body[off++] = (unsigned char)(id_len >> 8);
  body[off++] = (unsigned char)(id_len);
  memcpy(body + off, client_id, id_len);
  off += id_len;
  unsigned char pkt[160];
  size_t i = 0;
  pkt[i++] = 0x10;
  i += (size_t)encode_remaining(pkt + i, off);
  memcpy(pkt + i, body, off);
  i += off;
  if (write_all(fd, pkt, i) != 0)
    return -1;
  unsigned char ack[4];
  size_t got = 0;
  while (got < 4) {
    ssize_t k = read(fd, ack + got, 4 - got);
    if (k <= 0)
      return -1;
    got += (size_t)k;
  }
  if (ack[0] != 0x20 || ack[3] != 0)
    return -1;
  return 0;
}

static int read_exact(int fd, unsigned char *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t k = read(fd, buf + got, n - got);
    if (k <= 0)
      return -1;
    got += (size_t)k;
  }
  return 0;
}

static void *ack_reader(void *arg) {
  struct pub *p = arg;
  while (!p->stop) {
    unsigned char hdr[2];
    if (read_exact(p->fd, hdr, 2) != 0)
      break;
    size_t rem = (size_t)(hdr[1] & 0x7f);
    /* remaining length is one byte for PUBACK/PUBREC/PUBCOMP (2-byte payload) */
    unsigned shift = 7;
    size_t idx = 0;
    unsigned char extra;
    while (hdr[1] & 0x80) {
      if (read_exact(p->fd, &extra, 1) != 0)
        goto done;
      rem += (size_t)(extra & 0x7f) << shift;
      shift += 7;
      if (++idx > 3)
        goto done;
      if (!(extra & 0x80))
        break;
    }
    if (rem > 16) {
      /* unexpected; drain and exit */
      unsigned char junk[256];
      while (rem) {
        size_t chunk = rem > sizeof(junk) ? sizeof(junk) : rem;
        if (read_exact(p->fd, junk, chunk) != 0)
          goto done;
        rem -= chunk;
      }
      continue;
    }
    unsigned char body[16];
    if (rem && read_exact(p->fd, body, rem) != 0)
      break;
    unsigned type = hdr[0] >> 4;
    if (type == 4 || type == 7) { /* PUBACK / PUBCOMP */
      p->acked++;
      p->inflight--;
    } else if (type == 5 && rem >= 2) { /* PUBREC -> PUBREL */
      unsigned char rel[4] = {0x62, 2, body[0], body[1]};
      if (write_all(p->fd, rel, 4) != 0)
        break;
    }
  }
done:
  return NULL;
}

struct flood_arg {
  const char *host;
  int port;
  const char *topic;
  int index;
  int seconds;
  int window;
  int payload;
  unsigned long sent;
  unsigned long acked;
  int rc;
};

static void *flood_one(void *arg) {
  struct flood_arg *a = arg;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    a->rc = 1;
    return NULL;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  int bufsz = 1 << 20;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)a->port);
  if (inet_pton(AF_INET, a->host, &addr.sin_addr) != 1) {
    close(fd);
    a->rc = 2;
    return NULL;
  }
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    a->rc = 3;
    return NULL;
  }
  char client_id[64];
  snprintf(client_id, sizeof(client_id), "starve-pub-%d", a->index);
  if (mqtt_connect(fd, client_id) != 0) {
    close(fd);
    a->rc = 4;
    return NULL;
  }
  struct pub p;
  memset(&p, 0, sizeof(p));
  p.fd = fd;
  p.window = a->window;
  pthread_create(&p.ack_th, NULL, ack_reader, &p);

  size_t tlen = strlen(a->topic);
  /* packet: 0x32 + remaining + 2+topic + 2 pid + payload */
  size_t body_len = 2 + tlen + 2 + (size_t)a->payload;
  unsigned char remenc[4];
  int remn = encode_remaining(remenc, body_len);
  size_t pkt_len = 1 + (size_t)remn + body_len;
  unsigned char *pkt = malloc(pkt_len);
  if (!pkt) {
    p.stop = 1;
    shutdown(fd, SHUT_RDWR);
    pthread_join(p.ack_th, NULL);
    close(fd);
    a->rc = 5;
    return NULL;
  }
  size_t o = 0;
  pkt[o++] = 0x32;
  memcpy(pkt + o, remenc, (size_t)remn);
  o += (size_t)remn;
  pkt[o++] = (unsigned char)(tlen >> 8);
  pkt[o++] = (unsigned char)tlen;
  memcpy(pkt + o, a->topic, tlen);
  o += tlen;
  size_t pid_off = o;
  o += 2;
  memset(pkt + o, 'x', (size_t)a->payload);

  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  uint16_t pid = 1;
  unsigned long sent = 0;
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec >= a->seconds)
      break;
    while (p.inflight >= p.window) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec - start.tv_sec >= a->seconds)
        goto stop;
      usleep(50);
    }
    pkt[pid_off] = (unsigned char)(pid >> 8);
    pkt[pid_off + 1] = (unsigned char)pid;
    if (pid == 65535)
      pid = 1;
    else
      pid++;
    p.inflight++;
    if (write_all(fd, pkt, pkt_len) != 0)
      break;
    sent++;
  }
stop:
  /* brief drain so final PUBACKs can arrive */
  usleep(200 * 1000);
  p.stop = 1;
  shutdown(fd, SHUT_RD);
  pthread_join(p.ack_th, NULL);
  close(fd);
  free(pkt);
  a->sent = sent;
  a->acked = p.acked;
  a->rc = 0;
  return NULL;
}

int main(int argc, char **argv) {
  /* flood host port topic npub seconds window payload */
  if (argc != 8) {
    fprintf(stderr, "usage: %s host port topic npub seconds window payload\n", argv[0]);
    return 2;
  }
  const char *host = argv[1];
  int port = atoi(argv[2]);
  const char *topic = argv[3];
  int npub = atoi(argv[4]);
  int seconds = atoi(argv[5]);
  int window = atoi(argv[6]);
  int payload = atoi(argv[7]);
  if (npub < 1 || npub > 32 || seconds < 1 || window < 1 || payload < 1 || port <= 0) {
    fprintf(stderr, "bad args\n");
    return 2;
  }
  struct flood_arg *args = calloc((size_t)npub, sizeof(*args));
  pthread_t *ths = calloc((size_t)npub, sizeof(*ths));
  if (!args || !ths)
    return 1;
  for (int i = 0; i < npub; i++) {
    args[i].host = host;
    args[i].port = port;
    args[i].topic = topic;
    args[i].index = i;
    args[i].seconds = seconds;
    args[i].window = window;
    args[i].payload = payload;
    pthread_create(&ths[i], NULL, flood_one, &args[i]);
  }
  unsigned long sent = 0, acked = 0;
  int rc = 0;
  for (int i = 0; i < npub; i++) {
    pthread_join(ths[i], NULL);
    sent += args[i].sent;
    acked += args[i].acked;
    if (args[i].rc)
      rc = args[i].rc;
  }
  printf("flood sent=%lu acked=%lu rc=%d\n", sent, acked, rc);
  free(args);
  free(ths);
  return rc == 0 ? 0 : 1;
}
