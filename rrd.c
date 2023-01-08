#include <err.h>
#include <stdio.h>
#include <stdlib.h>

#include <rrd.h>
#include <rrd_client.h>

const char *addr = "/var/run/rrdcached.sock";
const char *filename = "/var/db/rrd/ping.rrd";

int main(void) {
  int ret;
  rrd_client_t *client = rrd_client_new(NULL);

  if (client == NULL) {
    errx(1, "rrd_client_new");
  }

  ret = rrd_client_connect(client, addr);
  if (ret) {
    errx(1, "rrd_client_connect");
  }

  for (;;) {
    int t = time(NULL);
    int r = rand() % 10;

    char buf[128];
    ret = snprintf(buf, sizeof(buf), "%d:%d", t, r);
    if (ret == sizeof(buf) - 1) {
      errx(1, "snprintf");
    }
    printf("%s\n", buf);

    const char *updates[1] = {buf};
    if ((ret = rrd_client_update(client, filename, 1, updates))) {
      errx(ret, "rrd_client_update");
    }
    if ((ret = rrd_client_flush(client, filename))) {
      errx(ret, "rrd_client_flushall");
    }
    sleep(1);
  }

  return 0;
}
