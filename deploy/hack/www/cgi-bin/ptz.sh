#!/bin/sh
# CGI: /cgi-bin/ptz.sh?dir=<up|down|left|right|stop>
#
# Press-and-hold semantics: a direction runs CONTINUOUSLY until the
# client sends stop (or this script's hard cap fires), like the stock
# app. The cap guarantees a dropped client can never leave the motor
# running: every move CGI schedules its own stop after MAXHOLD.
MAXHOLD=10

DIR="none"

for I in 1 2
do
    CONF="$(echo $QUERY_STRING | cut -d'&' -f$I | cut -d'=' -f1)"
    VAL="$(echo $QUERY_STRING | cut -d'&' -f$I | cut -d'=' -f2)"

    if [ "$CONF" = "dir" ] ; then
        DIR="$VAL"
    fi
done

case "$DIR" in
    up|down|left|right)
        /tmp/sd/hack/bin/ptz "$DIR"
        # Hard cap: never let the motor run more than MAXHOLD unattended.
        # Token-guarded so a cap from an OLDER move can never stop a
        # NEWER one (the classic stale-stop race: rapid presses each
        # schedule a cap, and without the token every cap kills the
        # latest move).
        echo $$ > /tmp/ptz_active
        # The redirect closes the sleeper's copy of the CGI stdout, so
        # the HTTP response completes NOW instead of hanging 10 s
        # (a hanging response stalls the browser's next request on
        # keep-alive connections).
        ( sleep $MAXHOLD
          [ "$(cat /tmp/ptz_active 2>/dev/null)" = "$$" ] && /tmp/sd/hack/bin/ptz stop
        ) >/dev/null 2>&1 &
        ;;
    stop)
        /tmp/sd/hack/bin/ptz stop
        rm -f /tmp/ptz_active
        ;;
esac

printf "Content-type: application/json\r\n\r\n"
printf "{\n"
printf "}\n"
