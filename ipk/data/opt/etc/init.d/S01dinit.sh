#!/bin/sh

PATH=/system/bin:/system/sbin:/opt/bin:/opt/sbin:/sbin:/bin:/usr/sbin:/usr/bin

PIDFILE="/opt/var/run/dinit.pid"
DINIT="/opt/sbin/dinit"
DINITD="/opt/etc/dinit.d"
LOGFILE="/blackbox/dinit.log"
HOME=/data

dinit_status ()
{
        [ -f $PIDFILE ] && [ -d /proc/`cat $PIDFILE` ]
}

start()
{
        $DINIT -u -d $DINITD -q -l $LOGFILE &
        echo $! > $PIDFILE
}

stop()
{
        kill `cat $PIDFILE`
}
case "$1" in
        start)
                if dinit_status
                then
                        echo dinit already running
                else
                        start
                fi
                ;;
        stop)
                if dinit_status
                then
                        stop
                else
                        echo dinit is not running
                fi
                ;;
        status)
                if dinit_status
                then
                        echo dinit already running
                else
                        echo dinit is not running
                fi
                ;;

        restart)
                stop
                sleep 3
                start
                ;;
        *)
                echo "Usage: $0 {start|stop|restart|status}"
                ;;
esac