#!/usr/local/bin/bash

# Graph for last 24 hours
doas /usr/local/bin/rrdtool graph latency_graph.png \
	-w 1000 -h 400 -a PNG \
	--slope-mode \
	--start -600 --end now \
	--font DEFAULT:7: \
	--title "bandwidth" \
	--watermark "`date`" \
	--vertical-label "bandwidth (bytes)" \
	--right-axis 0.001:0 \
	--x-grid MINUTE:10:HOUR:1:MINUTE:120:0:%R \
	--rigid \
	DEF:bytes_out=/var/db/rrd/pf.rrd:bytes_out:AVERAGE \
	DEF:bytes_in=/var/db/rrd/pf.rrd:bytes_in:AVERAGE \
	DEF:packets_out=/var/db/rrd/pf.rrd:packets_out:AVERAGE \
	DEF:packets_in=/var/db/rrd/pf.rrd:packets_in:AVERAGE \
	CDEF:neg_bytes_in=packets_out,-1000,* \
	CDEF:scale_packets_out=packets_out,1000,* \
	CDEF:scale_packets_in=packets_in,-1000,* \
	AREA:scale_packets_out#88ff88:"packets out" \
	AREA:scale_packets_in#8888ff:"packets in" \
	LINE1:bytes_out#00FF00:"bytes out" \
	LINE1:neg_bytes_in#0000FF:"bytes in" \

