#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <net/pfvar.h>
#include <netinet/in.h>
#include <sys/sysctl.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <rrd.h>
#include <rrd_client.h>

const char *pf_device = "/dev/pf";
const char *rrdcached_addr = "/var/run/rrd/rrdcached.sock";
char *rrd_filename = "/var/db/rrd";
struct subnetrepr repr;
int simple = 0, dryrun = 0, verbose = 0;

#define MAX_INTERFACES 16

struct pfstats {
  u_int64_t packets[2];
  u_int64_t bytes[2];
};

struct ifstat {
  char ifname[IFNAMSIZ];
  char rrd_filename[PATH_MAX];
  struct pfstats stats_acc;
  int last_ts;
};

static struct ifstat *ifstats = NULL;
static int if_count = 0;

void print_addr(sa_family_t af, void *src);

struct subnetrepr {
  struct pf_addr addr;
  struct pf_addr mask;
};

// ipv4 only
int subnetrepr_parse(struct subnetrepr *repr, char *addr) {
  char *p;
  const char *errstr;
  int prefix, ret_ga;
  struct addrinfo hints, *res, *iter;

  bzero(&hints, sizeof(hints));
  hints.ai_socktype = SOCK_DGRAM; /* dummy */
  hints.ai_flags = AI_NUMERICHOST;

  if ((p = strchr(addr, '/')) != NULL) {
    *p++ = '\0';
  }

  if ((ret_ga = getaddrinfo(addr, NULL, &hints, &res))) {
    errx(1, "getaddrinfo: %s", gai_strerror(ret_ga));
    /* NOTREACHED */
  }

  iter = res;
  while (iter->ai_next == NULL) {
    if (iter->ai_family == AF_INET) {
      repr->addr.v4 = ((struct sockaddr_in *)iter->ai_addr)->sin_addr;
      break;
    }
    iter = iter->ai_next;
  }
  freeaddrinfo(res);

  if (iter == NULL) {
    return -1;
  }

  repr->mask.v4.s_addr = (u_int32_t)0xffffffffffULL;
  if (p == NULL) {
    return 0;
  }

  prefix = strtonum(p, 0, res->ai_family == AF_INET6 ? 128 : 32, &errstr);
  if (errstr) {
    errx(1, "prefix is %s: %s", errstr, p);
  }

  repr->mask.v4.s_addr = htonl((u_int32_t)(0xffffffffffULL << (32 - prefix)));

  return 0;
}

// return 1 if matches
int subnetrepr_match(struct subnetrepr *repr, struct pf_addr *addr) {
  u_int32_t v4_repr_addr = (*(u_int32_t *)(&repr->addr.v4));
  u_int32_t v4_repr_mask = (*(u_int32_t *)(&repr->mask.v4));
  u_int32_t v4_addr = (*(u_int32_t *)(&addr->v4));

  if ((v4_repr_addr & v4_repr_mask) == (v4_addr & v4_repr_mask)) {
    return 1;
  }
  return 0;
}

void stats_fill(struct pfstats *stats, struct pfsync_state *state) {
  for (int i = 0; i < 2; i++) {
    bcopy(state->packets[i], &stats->packets[i], sizeof(u_int64_t));
    stats->packets[i] = betoh64(stats->packets[i]);

    bcopy(state->bytes[i], &stats->bytes[i], sizeof(u_int64_t));
    stats->bytes[i] = betoh64(stats->bytes[i]);
  }
}

void stats_sub(struct pfstats *s0, struct pfstats *s1) {
  for (int i = 0; i < 2; i++) {
    s0->packets[i] = s0->packets[i] - s1->packets[i];
    s0->bytes[i] = s0->bytes[i] - s1->bytes[i];
  }
}

void stats_add(struct pfstats *s0, struct pfstats *s1) {
  for (int i = 0; i < 2; i++) {
    s0->packets[i] = s0->packets[i] + s1->packets[i];
    s0->bytes[i] = s0->bytes[i] + s1->bytes[i];
  }
}

void stats_print(struct pfstats *s) {
  printf("bytes=%llu:%llu, packets=%llu:%llu", s->bytes[0], s->bytes[1],
         s->packets[0], s->packets[1]);
}

int pfctl_state_get(int dev, struct pfioc_states *ps) {
  char *inbuf = ps->ps_buf, *newinbuf = NULL;
  size_t len = 0;

  for (;;) {
    ps->ps_len = len;
    if (len) {
      newinbuf = realloc(inbuf, len);
      if (newinbuf == NULL)
        err(1, "realloc");
      ps->ps_buf = inbuf = newinbuf;
    }
    if (ioctl(dev, DIOCGETSTATES, ps) == -1)
      err(1, "DIOCGETSTATES");

    if (ps->ps_len + sizeof(struct pfioc_states) < len)
      break;
    if (len == 0 && ps->ps_len == 0)
      goto done;
    if (len == 0 && ps->ps_len != 0)
      len = ps->ps_len;
    if (ps->ps_len == 0)
      goto done; /* no states */
    len *= 2;
  }

  return 0;
done:
  return -1;
}

void print_addr(sa_family_t af, void *src) {
  static char buf[48];
  if (inet_ntop(af, src, buf, sizeof(buf)) == NULL)
    printf("?");
  else
    printf("%s", buf);
}

static void parse_interfaces(const char *list) {
  if (list == NULL || *list == '\0')
    return;

  char *tmp = strdup(list);
  if (tmp == NULL)
    err(1, "strdup");

  int count = 0;
  for (char *p = strtok(tmp, ","); p != NULL; p = strtok(NULL, ","))
    count++;

  free(tmp);
  tmp = strdup(list);
  if (tmp == NULL)
    err(1, "strdup");

  ifstats = calloc(count, sizeof(struct ifstat));
  if (ifstats == NULL)
    err(1, "calloc");

  int idx = 0;
  for (char *p = strtok(tmp, ","); p != NULL; p = strtok(NULL, ",")) {
    strlcpy(ifstats[idx].ifname, p, sizeof(ifstats[idx].ifname));
    snprintf(ifstats[idx].rrd_filename, sizeof(ifstats[idx].rrd_filename),
        "%s/pf-%s.rrd", rrd_filename, ifstats[idx].ifname);
    idx++;
  }

  rrd_filename = strdup(rrd_filename);
  rrd_filename = strcat(rrd_filename, "/pf.rrd");

  if_count = count;
  free(tmp);
}

int pfsync_state_cmp_id(const void *p0, const void *p1) {
  struct pfsync_state *s0 = (struct pfsync_state *)p0;
  struct pfsync_state *s1 = (struct pfsync_state *)p1;

  if (s0->id < s1->id) {
    return -1;
  }
  if (s0->id > s1->id) {
    return 1;
  }
  return 0;
}

void states_sort(struct pfioc_states *ps) {
  size_t n = ps->ps_len / sizeof(struct pfsync_state);
  qsort(ps->ps_buf, n, sizeof(struct pfsync_state), pfsync_state_cmp_id);
}

void state_print_key(struct pfsync_state_key *key, int idx) {
  print_addr(key->af, &key->addr[idx]);
  printf(":%d", ntohs(key->port[idx]));
}

void state_print_debug(struct pfsync_state *s) {
  struct pfsync_state_key *nk;
  if (s->direction == PF_OUT) {
    nk = &s->key[PF_SK_WIRE];
  } else {
    nk = &s->key[PF_SK_STACK];
  }

  printf("%llu %s ", s->id, s->ifname);
  if (s->proto == 6) {
    printf("tcp ");
  } else if (s->proto == 17) {
    printf("udp ");
  } else {
    printf("proto=%d ", s->proto);
  }

  state_print_key(nk, 1);

  if (s->direction == PF_OUT) {
    printf(" -> ");
  } else {
    printf(" <- ");
  }

  state_print_key(nk, 0);
  printf(" (");
  print_addr(AF_INET, &s->rt_addr);
  printf(") ");

  struct pfstats stats;
  stats_fill(&stats, s);
  stats_print(&stats);
  printf("\n");
}

// return 1 if non-NATed simple flow
int state_simple_idx(struct pfsync_state *s, int idx) {
  struct pfsync_state_key *sk, *nk;

  sk = &s->key[PF_SK_STACK];
  nk = &s->key[PF_SK_WIRE];

  if (sk->af != nk->af || PF_ANEQ(&sk->addr[idx], &nk->addr[idx], sk->af) ||
      sk->port[idx] != nk->port[idx] || sk->rdomain != nk->rdomain) {
    return 0;
  }
  return 1;
}

// return 1 if non-nated simple flow
int state_simple(struct pfsync_state *s) {
  if (s->af != AF_INET) {
    return 0;
  }
  if (state_simple_idx(s, 0) && state_simple_idx(s, 1)) {
    return 1;
  }
  return 0;
}

void acct_for(struct pf_addr *src, struct pf_addr *dst, u_int64_t bytes,
              u_int64_t packets, struct pfstats *stats) {
  int src_local = subnetrepr_match(&repr, src);
  int dst_local = subnetrepr_match(&repr, dst);

  if (src_local == dst_local) {
    return;
  }


  if (src_local) {
    stats->bytes[0] += bytes;
    stats->packets[0] += packets;
  } else {
    stats->bytes[1] += bytes;
    stats->packets[1] += packets;
  }
}

void states_cmp(struct pfstats *stats, struct pfsync_state *cur,
                struct pfsync_state *prev) {
  struct pfsync_state *s = cur != NULL ? cur : prev;

  if (simple != state_simple(s)) {
    return;
  }

  if (cur == NULL) {
    // printf("DEL ");
    // state_print_debug(prev);
    return;
  }

  struct pfstats stats_cur, stats_prev;
  stats_fill(&stats_cur, cur);

  if (prev == NULL) {
    // printf("ADD ");
    // state_print_debug(cur);

    memset(&stats_prev, 0, sizeof(struct pfstats));
  } else {
    stats_fill(&stats_prev, prev);
  }
  stats_sub(&stats_cur, &stats_prev);

  int afto = s->key[PF_SK_STACK].af == s->key[PF_SK_WIRE].af ? 0 : 1;
  int dir = afto ? PF_OUT : s->direction;

  uint64_t bytes, packets;
  struct pfsync_state_key *ks;

  bytes = (dir == PF_OUT) ? stats_cur.bytes[0] : stats_cur.bytes[1];
  packets = (dir == PF_OUT) ? stats_cur.packets[0] : stats_cur.packets[1];
  if (bytes > 0) {
    ks = &s->key[afto ? PF_SK_STACK : PF_SK_WIRE];
    acct_for(&ks->addr[1], &ks->addr[0], bytes, packets, stats);
    for (int i = 0; i < if_count; i++) {
      if (strcmp(ifstats[i].ifname, s->ifname) == 0) {
        acct_for(&ks->addr[1], &ks->addr[0], bytes, packets,
                 &ifstats[i].stats_acc);
      }
    }
  }

  bytes = (dir == PF_OUT) ? stats_cur.bytes[1] : stats_cur.bytes[0];
  packets = (dir == PF_OUT) ? stats_cur.packets[1] : stats_cur.packets[0];
  if (bytes > 0) {
    ks = &s->key[PF_SK_STACK];
    acct_for(&ks->addr[0], &ks->addr[1], bytes, packets, stats);
    for (int i = 0; i < if_count; i++) {
      if (strcmp(ifstats[i].ifname, s->ifname) == 0) {
        acct_for(&ks->addr[0], &ks->addr[1], bytes, packets,
                 &ifstats[i].stats_acc);
      }
    }
  }
}

int step(int dev, struct pfioc_states *ps, struct pfioc_states *ps_prev,
         struct pfstats *stats) {
  if (pfctl_state_get(dev, ps) == -1) {
    err(1, "pfctl_state_get");
  }
  states_sort(ps);

  size_t sz = sizeof(struct pfsync_state);

  size_t n = ps->ps_len / sz;
  size_t n_prev = ps_prev->ps_len / sz;
  size_t i = 0, i_prev = 0;

  // compare sorted array
  while (i < n || i_prev < n_prev) {
    struct pfsync_state *s = (struct pfsync_state *)(ps->ps_buf + i * sz);
    struct pfsync_state *s_prev =
        (struct pfsync_state *)(ps_prev->ps_buf + i_prev * sz);
    if (i == n) {
      states_cmp(stats, NULL, s_prev);
      i_prev += 1;
    } else if (i_prev == n_prev) {
      states_cmp(stats, s, NULL);
      i += 1;
    } else if (s->id > s_prev->id) {
      states_cmp(stats, NULL, s_prev);
      i_prev += 1;
    } else if (s->id < s_prev->id) {
      states_cmp(stats, s, NULL);
      i += 1;
    } else {
      assert(s->id == s_prev->id);

      states_cmp(stats, s, s_prev);
      i += 1;
      i_prev += 1;
    }
  }

  return 0;
}

int send_msg(int sockfd, struct sockaddr_in *addr, const void *data,
             size_t datalen) {
  return sendto(sockfd, data, datalen, 0, (const struct sockaddr *)addr,
                sizeof(struct sockaddr_in));
}

void send_counter(int sockfd, struct sockaddr_in *addr, const char *name,
                  size_t counter) {
  char buf[1024];
  int ret = snprintf(buf, sizeof(buf), "%s:%lu|c\n", name, counter);
  if (ret == sizeof(buf) - 1) {
    err(1, "snprintf");
  }
  ret = send_msg(sockfd, addr, buf, ret);
  if (ret == -1) {
    err(1, "send_msg");
  }
}

__dead void usage(void) {
  extern char *__progname;

  fprintf(stderr,
          "usage: %s [-n network] [-r rrdfile] [-i iface,iface...] [-d]\n",
          __progname);
  exit(1);
}

void rrd_update_stats(rrd_client_t *client, const char *filename,
                      struct pfstats stats, int ts, int *last_ts) {
  int ret;
  char buf[1024];

  if (last_ts && ts < *last_ts)
    return;
  if (last_ts)
    *last_ts = ts;

  ret = snprintf(buf, sizeof(buf), "%d:%llu:%llu:%llu:%llu", ts, stats.bytes[0],
                 stats.bytes[1], stats.packets[0], stats.packets[1]);

  if (ret == sizeof(buf) - 1) {
    errx(1, "snprintf");
  }

  if (verbose) {
    printf("%s: %s\n", filename, buf);
  }
  
  if (dryrun) {
    return;
  }

  const char *updates[1] = {buf};
  if ((ret = rrd_client_update(client, filename, 1, updates))) {
    warn("rrd_client_update: %d, %s", ret, rrd_get_error());
  }
  if ((ret = rrd_client_flush(client, filename))) {
    errx(ret, "rrd_client_flush");
  }
}

int main(int argc, char *argv[]) {
  char *addr = NULL;
  char *iface_list = NULL;
  int ret, ch;
  int daemonize = 1;

  while ((ch = getopt(argc, argv, "sfdvr:n:i:")) != -1) {
    switch (ch) {
    case 's':
      simple = 1;
      continue;
    case 'f':
      daemonize = 0;
      continue;
    case 'd':
      dryrun = 1;
      continue;
    case 'v':
      verbose = 1;
      continue;
    case 'r':
      rrd_filename = strdup(optarg);
      continue;
    case 'i':
      iface_list = strdup(optarg);
      continue;
    case 'n':
      addr = strdup(optarg);
      continue;
    default:
      /* NOTREACHED */
      usage();
    }
  }

  if (addr == NULL) {
    usage();
  }

  memset(&repr, 0, sizeof(struct subnetrepr));
  subnetrepr_parse(&repr, addr);
  parse_interfaces(iface_list);

  int dev = -1;
  int mode = O_RDONLY;
  dev = open(pf_device, mode);
  if (dev == -1) {
    err(1, "%s", pf_device);
  }

  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    err(1, "socket");
  }

  rrd_client_t *client = rrd_client_new(NULL);

  if (client == NULL) {
    errx(1, "rrd_client_new");
  }

  ret = rrd_client_connect(client, rrdcached_addr);
  if (ret) {
    errx(1, "rrd_client_connect");
  }

  struct pfioc_states ps0, ps1;
  memset(&ps0, 0, sizeof(ps0));
  memset(&ps1, 0, sizeof(ps1));

  struct pfioc_states *ps = &ps0, *ps_prev = &ps1, *tmp = NULL;

  struct pfstats stats, stats_acc;
  memset(&stats_acc, 0, sizeof(stats_acc));
  int last_ts_total = 0;

  // warm up
  if (pfctl_state_get(dev, ps_prev) == -1) {
    err(1, "pfctl_state_get");
  }
  states_sort(ps_prev);

  if (daemonize && (ret = daemon(0, 0))) {
    errx(1, "daemon");
  }

  struct timespec ts;
  struct timeval tv, tv_now, tv_interval, tv_sleep;

  gettimeofday(&tv, NULL);
  tv_interval.tv_sec = 1;
  tv_interval.tv_usec = 0;

  timeradd(&tv, &tv_interval, &tv);

  for (;;) {
    memset(&stats, 0, sizeof(stats));
    step(dev, ps, ps_prev, &stats);

    gettimeofday(&tv_now, NULL);

    if (!daemonize) {
      printf("%lld.%06ld ", tv_now.tv_sec, tv_now.tv_usec);
      stats_print(&stats);
      printf("\n");
    }

    stats_add(&stats_acc, &stats);

    rrd_update_stats(client, rrd_filename, stats_acc, tv_now.tv_sec,
                     &last_ts_total);
    for (int i = 0; i < if_count; i++)
      rrd_update_stats(client, ifstats[i].rrd_filename, ifstats[i].stats_acc,
                       tv_now.tv_sec, &ifstats[i].last_ts);

    // swap buffer
    tmp = ps;
    ps = ps_prev;
    ps_prev = tmp;

    // wait
    gettimeofday(&tv_now, NULL);
    if (timercmp(&tv, &tv_now, >)) {
      timersub(&tv, &tv_now, &tv_sleep);
      TIMEVAL_TO_TIMESPEC(&tv_sleep, &ts);

      while ((ret = nanosleep(&ts, NULL))) {
        if (errno != EINTR) {
          errx(1, "nanosleep: %d", ret);
        }
      }
    } else {
      warn("tv_now(%lld.%06ld) > tv(%lld.%06ld)", tv_now.tv_sec, tv_now.tv_usec,
           tv.tv_sec, tv.tv_usec);
    }

    timeradd(&tv, &tv_interval, &tv);
  }

  free(ps0.ps_buf);
  free(ps1.ps_buf);

  close(sockfd);
  close(dev);
  return 0;
}
