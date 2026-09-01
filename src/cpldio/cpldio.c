/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * cpldio - fire one /dev/cpld_periph ioctl from the shell.
 *
 * Debugging the white-LED (flashlight) control path: dispatch runs
 * ioctl(fd, 0x701c/0x701b/0x7021/0x7022/0x7018/0x7019, ...) on this
 * device for the app's lamp feature (see PLAN.md flashlight RE). This
 * tool bypasses dispatch's guards to test the ioctls directly.
 *
 * usage: cpldio REQ [ARG]    (both hex; arg defaults 0)
 *
 * GPL-3.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
    unsigned long req, arg = 0;
    int fd, r;

    if (argc < 2) {
        fprintf(stderr, "usage: cpldio REQ [ARG]   (hex)\n");
        return 2;
    }
    req = strtoul(argv[1], NULL, 0);
    if (argc > 2)
        arg = strtoul(argv[2], NULL, 0);
    fd = open("/dev/cpld_periph", O_RDWR);
    if (fd < 0) {
        perror("open /dev/cpld_periph");
        return 1;
    }
    r = ioctl(fd, req, arg);
    printf("ioctl(fd, 0x%lx, %lu) -> %d\n", req, arg, r);
    close(fd);
    return 0;
}
