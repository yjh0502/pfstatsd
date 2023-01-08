rrdtool \
	create \
	pf.rrd \
	--step 1 \
	DS:bytes_in:DERIVE:10:U:U \
	DS:bytes_out:DERIVE:10:U:U \
	DS:packets_in:DERIVE:10:U:U \
	DS:packets_out:DERIVE:10:U:U \
	RRA:AVERAGE:0.5:1:86400 \
	RRA:AVERAGE:0.5:10:86400 \
	RRA:AVERAGE:0.5:60:40320 \
	RRA:AVERAGE:0.5:3600:87600
