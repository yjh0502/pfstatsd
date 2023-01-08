#!/usr/bin/env sh

OUTDIR="/var/www/html/"

graph() {
	NAME="$1"
	START="$2"
	/usr/local/bin/rrdtool graph \
		-d /var/run/rrd/rrdcached.sock \
		"${OUTDIR}/graph-${NAME}.png" \
		-w 600 -h 200 -a PNG \
		--slope-mode \
		--start "-${START}" --end now \
		--font DEFAULT:8: \
		--title "bandwidth - ${NAME}" \
		--watermark "`date`" \
		--vertical-label "bandwidth" \
		--right-axis-label "packet count" \
		--right-axis 0.001:0 \
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
		LINE1:neg_bytes_in#0000FF:"bytes in"
}

graph 1m 60
graph 10m 600
graph 1h 3600
graph 1d 86400
