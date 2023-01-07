#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <net/if.h>
#include <netinet/in.h>
#include <net/pfvar.h>
#include <arpa/inet.h>
#include <sys/sysctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdarg.h>
#include <libgen.h>
#include <assert.h>

char *pf_device = "/dev/pf";

int
pfctl_state_get(int dev, struct pfioc_states *ps)
{
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
			goto done;	/* no states */
		len *= 2;
	}

	return 0;
done:
	return -1;
}

void
print_addr(sa_family_t af, void *src) {
	static char buf[48];
	if (inet_ntop(af, src, buf, sizeof(buf)) == NULL)
		printf("?");
	else
		printf("%s", buf);
}

int
pfsync_state_cmp_id(const void *p0, const void *p1) {
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

void
states_sort(struct pfioc_states *ps)
{
	size_t n = ps->ps_len / sizeof(struct pfsync_state);
	qsort(ps->ps_buf, n, sizeof(struct pfsync_state), pfsync_state_cmp_id);
}

void
state_print_debug(struct pfsync_state *s) {
	printf("%llu %s ", s->id, s->ifname);
	if (s->proto == 6) {
		printf("tcp ");
	} else if (s->proto == 17) {
		printf("udp ");
	} else {
		printf("proto=%d ", s->proto);
	}

	print_addr(s->af, &s->key[0].addr[0]);
	printf(":%d", ntohs(s->key[0].port[0]));
	printf(" / ");
	print_addr(s->af, &s->key[0].addr[1]);
	printf(":%d", ntohs(s->key[0].port[1]));

	printf("\n");

}

struct pfstats {
	u_int64_t packets[2];
	u_int64_t bytes[2];
};

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
	printf("bytes=%llu:%llu, packets=%llu:%llu\n", s->bytes[0], s->bytes[1], s->packets[0], s->packets[1]);
}

void states_cmp(struct pfstats *stats, struct pfsync_state *cur, struct pfsync_state *prev) {
	if (cur == NULL) {
		printf("DEL ");
		state_print_debug(prev);
		return;
	}
	if (prev == NULL) {
		printf("ADD ");
		state_print_debug(cur);
		return;
	}

	struct pfstats stats_cur, stats_prev;
	stats_fill(&stats_cur, cur);
	stats_fill(&stats_prev, prev);

	stats_sub(&stats_cur, &stats_prev);
	stats_add(stats, &stats_cur);
}

int step(int dev, struct pfioc_states *ps, struct pfioc_states *ps_prev) {
	if (pfctl_state_get(dev, ps) == -1) {
		err(1, "pfctl_state_get");
	}
	states_sort(ps);

	size_t sz = sizeof(struct pfsync_state);

	size_t n = ps->ps_len / sz;
	size_t n_prev = ps_prev->ps_len / sz;
	size_t i = 0, i_prev = 0;

	struct pfstats stats;
	memset(&stats, 0, sizeof(stats));

	// compare sorted array
	while (i < n || i_prev < n_prev) {
		struct pfsync_state *s = (struct pfsync_state *)(ps->ps_buf + i * sz);
		struct pfsync_state *s_prev = (struct pfsync_state *)(ps_prev->ps_buf + i_prev * sz);
		if (i == n) {
			states_cmp(&stats, NULL, s_prev);
			i_prev += 1;
		} else if (i_prev == n_prev) {
			states_cmp(&stats, s, NULL);
			i += 1;
		} else if (s->id > s_prev->id) {
			states_cmp(&stats, NULL, s_prev);
			i_prev += 1;
		} else if (s->id < s_prev->id) {
			states_cmp(&stats, s, NULL);
			i += 1;
		} else {
			assert(s->id == s_prev->id);

			states_cmp(&stats, s, s_prev);
			i += 1;
			i_prev += 1;
		}
	}

	stats_print(&stats);

	return 0;
}

int main(void) {
	int dev = -1;
	int mode = O_RDONLY;
	dev = open(pf_device, mode);
	if (dev == -1) {
		err(1, "%s", pf_device);
	}

	struct pfioc_states ps0, ps1;
	memset(&ps0, 0, sizeof(ps0));
	memset(&ps1, 0, sizeof(ps1));

	struct pfioc_states *ps = &ps0, *ps_prev = &ps1, *tmp = NULL;

	for (;;) {
		step(dev, ps, ps_prev);

		// swap buffer
		tmp = ps;
		ps = ps_prev;
		ps_prev = tmp;

		sleep(1);
	}

	free(ps0.ps_buf);
	free(ps1.ps_buf);

	close(dev);
	return 0;
}
