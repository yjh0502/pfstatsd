
pfstatsd: pfstatsd.c
	cc -Wall -Wextra -Werror -I /usr/local/include -lm /usr/local/lib/librrd.a pfstatsd.c -o pfstatsd

rrd: rrd.c
	cc -Wall -Wextra -Werror -I /usr/local/include -lm /usr/local/lib/librrd.a rrd.c -o rrd

format:
	clang-format -i pfstatsd.c
