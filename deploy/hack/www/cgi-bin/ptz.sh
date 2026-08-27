#!/bin/sh
# CGI: /cgi-bin/ptz.sh?dir=<up|down|left|right|stop>[&time=<sec>]
# Upstream yi-hack-v5's ptz.sh pattern: move for a bounded hold, then
# stop, so a dropped client can never leave the motor running.
#
# Called as: ptz.sh?dir=right&time=0.5

DIR="none"
TIME="0.3"

for I in 1 2
do
    CONF="$(echo $QUERY_STRING | cut -d'&' -f$I | cut -d'=' -f1)"
    VAL="$(echo $QUERY_STRING | cut -d'&' -f$I | cut -d'=' -f2)"

    if [ "$CONF" = "dir" ] ; then
        DIR="$VAL"
    elif [ "$CONF" = "time" ] ; then
        TIME="$VAL"
    fi
done

case "$DIR" in
    up|down|left|right|stop) ;;
    *) DIR="none" ;;
esac

if [ "$DIR" != "none" ] ; then
    /tmp/sd/hack/bin/ptz "$DIR"
    sleep "$TIME"
    /tmp/sd/hack/bin/ptz stop
fi

printf "Content-type: application/json\r\n\r\n"
printf "{\n"
printf "}\n"
