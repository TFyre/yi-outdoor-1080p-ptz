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

# --- dropbear SSH ---
# Host key persists on the SD across reboots. dropbearkey cannot write
# directly to vfat (its chmod fails with EPERM), so generate in tmpfs and
# copy onto the SD with cat (plain write, no chmod).
if [ ! -f /tmp/sd/yi-hack-v5/etc/dropbear/ecdsa.key ]; then
  mkdir -p /tmp/sd/yi-hack-v5/etc/dropbear /tmp/dropbear
  /tmp/sd/yi-hack-v5/bin/dropbearmulti dropbearkey -t ecdsa -f /tmp/dropbear/ecdsa.key
  cat /tmp/dropbear/ecdsa.key > /tmp/sd/yi-hack-v5/etc/dropbear/ecdsa.key
fi
mkdir -p /root/.ssh
echo "ssh-rsa AAAA...redacted (personal key; see deploy/hack/root/.ssh/authorized_keys.example)" > /root/.ssh/authorized_keys
chmod 700 /root/.ssh
chmod 600 /root/.ssh/authorized_keys
nohup /tmp/sd/yi-hack-v5/bin/dropbearmulti dropbear -r /tmp/sd/yi-hack-v5/etc/dropbear/ecdsa.key -E -p 2222 >/tmp/db.log 2>&1 &
