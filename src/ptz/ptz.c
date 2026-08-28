/*
 * ptz - drive the camera's PTZ through the app's OWN IPC (its POSIX
 * mqueue family), so the stock app keeps handling motor calibration,
 * limits, and position persistence.
 *
 * Command envelopes: upstream yi-hack-v5's ipc_cmd protocol (same app
 * family - their ipc_cmd.h strings) cross-checked against dispatch's
 * reverse-engineered forward (PLAN.md's PTZ section, analysis/app/):
 *   MOVE = {u32 1, u32 type, u16 0x4006, u16 0x4006, u32 24, u32 dir, u32 0}
 *          dir: 1=up 2=down 3=left 4=right
 *   STOP = {u32 1, u32 type, u16 0x4007, u16 0x0001, u32 0}          (16 B)
 *   PRESET_GOTO = {1, type, 0x4002, 0x0001, 4, index}
 *   PRESET_ADD  = {1, type, 0x4000, 0x0001, 0}                      (16 B)
 *   PRESET_REM  = {1, type, 0x4001, 0x0001, 4, index}
 * VERIFIED LIVE on the camera (2026-08-28): the default queue
 * /ipc_dispatch and type 8 drive the motor in all four directions -
 * the user fired up/down/left/right with 1 s holds and stops, all
 * worked natively (dispatch received the envelope and ran its own
 * motor path: UART to the CPLD board).
 *
 * usage: ptz [-q QUEUE] [-t TYPE] up|down|left|right|stop|add|del N|goto N
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <mqueue.h>

static const char *g_queue = "/ipc_dispatch";
static unsigned g_type = 8;

static void put32(unsigned char *p, unsigned v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}

/* envelope head shared by all commands: {1, type, cmd, cmd2} */
static int dry_run;

static int send_msg(const unsigned char *payload, size_t plen,
                    unsigned cmd, unsigned cmd2, unsigned extra)
{
    unsigned char msg[64];
    size_t n = 0, i;

    memset(msg, 0, sizeof(msg));
    put32(msg + 0, 1);
    put32(msg + 4, g_type);
    put16(msg + 8, cmd);
    put16(msg + 10, cmd2);
    n = 12;
    if (plen) {
        memcpy(msg + n, payload, plen);
        n += plen;
    }
    if (extra) {
        put32(msg + n, extra);
        n += 4;
    }

    if (dry_run) {
        for (i = 0; i < n; i++)
            printf("%02x ", msg[i]);
        printf("(queue %s, %u bytes)\n", g_queue, (unsigned)n);
        return 0;
    }

    mqd_t mq = mq_open(g_queue, O_WRONLY);
    if (mq == (mqd_t)-1) {
        perror("mq_open");
        fprintf(stderr, "ptz: queue %s not available - is the app up?\n",
                g_queue);
        return 1;
    }
    if (mq_send(mq, (char *)msg, n, 0) != 0) {
        perror("mq_send");
        mq_close(mq);
        return 1;
    }
    mq_close(mq);
    return 0;
}

static int cmd_move(unsigned dir)
{
    unsigned char p[12];

    put32(p + 0, 24);      /* message size field */
    put32(p + 4, dir);
    put32(p + 8, 0);
    return send_msg(p, sizeof(p), 0x4006, 0x4006, 0);
}

static int cmd_stop(void)
{
    unsigned char z[4] = {0, 0, 0, 0};

    /* upstream's IPC_MOVE_STOP is 16 bytes: {1, type, 0x4007, 0x0001, 0} */
    return send_msg(z, sizeof(z), 0x4007, 0x0001, 0);
}

static int cmd_preset(unsigned cmd, int index)
{
    /* add:  {1, type, 0x4000, 0x0001, 0}              (16 B)
     * goto: {1, type, 0x4002, 0x0001, 4, index}      (20 B)
     * del:  {1, type, 0x4001, 0x0001, 4, index}      (20 B) */
    if (cmd == 0x4000) {
        unsigned char z[4] = {0, 0, 0, 0};
        return send_msg(z, sizeof(z), 0x4000, 0x0001, 0);
    }
    unsigned char p[8];
    put32(p, 4);
    put32(p + 4, (unsigned)index);
    return send_msg(p, sizeof(p), cmd, 0x0001, 0);
}

int main(int argc, char **argv)
{
    const char *cmd;
    int arg = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q") && i + 1 < argc)
            g_queue = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            g_type = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-x"))
            dry_run = 1;   /* print the envelope hex instead of sending */
        else
            break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: ptz [-x] [-q QUEUE] [-t TYPE] "
                        "up|down|left|right|stop|add|del N|goto N\n");
        return 2;
    }
    cmd = argv[i];
    if (i + 1 < argc)
        arg = atoi(argv[i + 1]);

    if (!strcmp(cmd, "up"))       return cmd_move(1);
    if (!strcmp(cmd, "down"))     return cmd_move(2);
    if (!strcmp(cmd, "left"))     return cmd_move(3);
    if (!strcmp(cmd, "right"))    return cmd_move(4);
    if (!strcmp(cmd, "stop"))     return cmd_stop();
    if (!strcmp(cmd, "add"))      return cmd_preset(0x4000, 0);
    if (!strcmp(cmd, "del"))      return cmd_preset(0x4001, arg);
    if (!strcmp(cmd, "goto"))     return cmd_preset(0x4002, arg);

    fprintf(stderr, "ptz: unknown command '%s'\n", cmd);
    return 2;
}
