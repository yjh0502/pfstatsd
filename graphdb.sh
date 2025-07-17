#!/usr/local/bin/bash

OUTDIR="/var/www/html/"

graph0() {
	NAME="$1"
	START="$2"
	PREFIX="$3"
	FILE="$4"
	/usr/local/bin/rrdtool graph \
		-d /var/run/rrd/rrdcached.sock \
		"${OUTDIR}/${PREFIX}-${NAME}.png" \
		-w 400 -h 200 -a PNG \
		--slope-mode \
		--start "-${START}" --end now \
		--font DEFAULT:8: \
		--title "bandwidth - ${PREFIX} - ${NAME}" \
		--watermark "`date`" \
		--vertical-label "bytes (bytes)" \
		--right-axis-label "packets (count)" \
		--right-axis 0.001:0 \
		--rigid \
		DEF:bytes_out="${FILE}":bytes_out:AVERAGE \
		DEF:bytes_in="${FILE}":bytes_in:AVERAGE \
		DEF:packets_out="${FILE}":packets_out:AVERAGE \
		DEF:packets_in="${FILE}":packets_in:AVERAGE \
		CDEF:neg_bytes_in=packets_out,-1000,* \
		CDEF:scale_packets_out=packets_out,1000,* \
		CDEF:scale_packets_in=packets_in,-1000,* \
		AREA:scale_packets_out#88ff88:"packets in" \
		AREA:scale_packets_in#8888ff:"packets out" \
		LINE1:bytes_out#00FF00:"bytes in" \
		LINE1:neg_bytes_in#0000FF:"bytes out"
}

graph() {
	PREFIX="$1"
	FILE="$2"
	graph0 3m 150 "$PREFIX" "$FILE"
	graph0 15m 900 "$PREFIX" "$FILE"
	graph0 1h 3600 "$PREFIX" "$FILE"
	graph0 1d 86400 "$PREFIX" "$FILE"
	graph0 28d 2422000 "$PREFIX" "$FILE"
}

graph graph /var/db/rrd/pf.rrd
