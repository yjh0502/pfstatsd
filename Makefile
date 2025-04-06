CFLAGS = -O2 -Wall -Wextra -Werror -I /usr/local/include -lm /usr/local/lib/librrd.a

pfstatsd: pfstatsd.c
	cc $(CFLAGS) pfstatsd.c -o pfstatsd

rrd: rrd.c
	cc $(CFLAGS) rrd.c -o rrd

clean:
	rm -f pfstatsd rrd

format:
	clang-format -i pfstatsd.c
	clang-format -i rrd.c
