CFLAGS = -O2 -Wall -Wextra -Werror -I /usr/local/include -lm /usr/local/lib/librrd.a

pfstatsd: pfstatsd.c
	cc $(CFLAGS) pfstatsd.c -o pfstatsd

rrd: rrd.c
	cc $(CFLAGS) rrd.c -o rrd

format:
	clang-format -i pfstatsd.c
