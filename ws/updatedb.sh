#!/usr/local/bin/bash
#
### set the paths
command="/sbin/ping -q -n -c 3"
gawk="/usr/local/bin/gawk"
rrdtool="/usr/local/bin/rrdtool"
hosttoping="10.0.7.1"

### data collection routine
get_data() {
    local output=$($command $1 2>&1)
    local method=$(echo "$output" | $gawk '
        BEGIN {pl=100; rtt=0.1}
        /packets transmitted/ {
            match($0, /([0-9]+)% packet loss/, datapl)
            pl=datapl[1]
        }
        /min\/avg\/max/ {
            match($4, /(.*)\/(.*)\/(.*)\/(.*)/, datartt)
            rtt=datartt[2]
        }
        END {print pl ":" rtt}
        ')
    RETURN_DATA=$method
}

### collect the data
get_data $hosttoping

### update the database
$rrdtool update latency_db.rrd --template pl:rtt N:$RETURN_DATA
