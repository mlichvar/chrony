#!/bin/sh

#  chronylogrotate.sh
#  ChronyControl
#
#  Created by Bryan Christianson on 12/07/15.
#

LOGDIR=/var/log/chrony

if [ ! -e "$LOGDIR" ]; then
  echo "missing directory: $LOGDIR"
  exit 1
fi

cd $LOGDIR

rotate () {
  prefix=$1

  rm -f $prefix.log.10

  for (( count=9; count>= 0; count-- ))
  do
    next=$(( $count+1 ))
    if [ -f $prefix.log.$count ]; then
      mv $prefix.log.$count $prefix.log.$next
    fi
  done

  if [ -f $prefix.log ]; then
    mv $prefix.log $prefix.log.0
  fi
}

rotate measurements
rotate statistics
rotate tracking

#
# signal chronyd via chronyc

/usr/local/bin/chronyc -a -f /etc/chrony.d/chrony.conf cyclelogs > /dev/null

exit $?