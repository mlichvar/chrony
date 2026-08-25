#!/bin/bash
# This script starts chronyd in multiple instances, one client and multiple
# servers (one per CPU by default) sharing one server port, in order to
# increase the maximum rate of NTP requests that can be handled as a server.

servers=${SERVERS:-$(nproc)}
chronyd="/usr/sbin/chronyd"
rundir="/var/run/chrony"
localport=11123
localpoll=0

trap terminate EXIT

terminate()
{
	kill $(jobs -p) &> /dev/null
	wait
	echo Exiting
}

if [ "$(uname -s)" != "Linux" ]; then
	echo "Only Linux is supported"
	exit 1
fi

conf=""
for c in /etc/chrony.conf /etc/chrony/chrony.conf; do
	[ -f "$c" ] || continue
	conf=$c
	break
done

if [ -z "$conf" ]; then
	echo "Missing chrony.conf"
	exit 1
fi

if ! [ -x "$chronyd" ]; then
	echo "Missing chronyd"
	exit 1
fi

version=$("$chronyd" --version | grep -o -E '[0-9]+\.[0-9]+' | head -n 1)

case "$version" in
	1.*|2.*|3.*)
		echo "chrony version $version too old to run multiple instances"
		exit 1
		;;
	4.0)
		opts=""
		;;
	4.1)
		opts="xleave copy"
		;;
	*)
		opts="xleave copy extfield F323"
		;;
esac

for i in $(seq 1 "$servers"); do
	echo "Starting server instance #$i"
	"$chronyd" -n -x "$@" \
		"server 127.0.0.1 port $localport minpoll $localpoll maxpoll $localpoll $opts" \
		"allow" \
		"cmdport 0" \
		"bindcmdaddress $rundir/chronyd-server$i.sock" \
		"pidfile $rundir/chronyd-server$i.pid" &
done

echo "Starting client instance"
"$chronyd" -n "$@" \
	"include $conf" \
	"port $localport" \
	"bindaddress 127.0.0.1" \
	"allow 127.0.0.1"
