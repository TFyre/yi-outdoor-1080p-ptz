#udhcpc -i wlan0 -b -s /backup/script/default.script &
#sleep 10
#/bin/busybox telnetd -l/bin/ash -p9999 &
/bin/busybox telnetd -l/bin/bash -p9998 &

if [ -f /tmp/sd/yi-hack-v5/busybox/busybox ]; then
  cp /tmp/sd/yi-hack-v5/busybox/busybox /bin/busybox1.36.1
  rm -f /bin/ash
  /bin/busybox1.36.1 --install -s /bin
fi

/bin/busybox1.36.1 telnetd -l/bin/ash -p9999 &

export PATH=$PATH:/tmp/sd/yi-hack-v5/bin/
alias vim=vi
