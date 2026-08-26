/* peek0401.c - where does the era's IDR live? Print the chain
 * records' payloads in full (0x0422/0x0404/0x0401) and count EVERY
 * NAL inside every 0x0400 payload (multi-NAL payloads would hide an
 * IDR behind the first P slice). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define RING     "/dev/shm/fshare_frame_buf"
#define DATA_OFF 300u
#define DATA_SZ  0x1B4000u

static volatile unsigned char *buf;
static size_t buf_size;

static uint32_t rd32(uint32_t p)
{
    if (p + 3 >= buf_size)
        return 0;
    return (uint32_t)buf[p] | ((uint32_t)buf[p + 1] << 8) |
           ((uint32_t)buf[p + 2] << 16) | ((uint32_t)buf[p + 3] << 24);
}

static void ring_read(uint32_t off, void *dst, size_t n)
{
    size_t first = DATA_SZ - off;
    if (first > n)
        first = n;
    memcpy(dst, (const void *)(buf + DATA_OFF + off), first);
    if (first < n)
        memcpy((uint8_t *)dst + first, (const void *)(buf + DATA_OFF),
               n - first);
}

int main(void)
{
    int fd;
    struct stat st;
    uint32_t tail, valid;
    int left;
    unsigned n0400 = 0, ntl_hist[32];
    int chain_shown = 0;

    memset(ntl_hist, 0, sizeof(ntl_hist));

    fd = open(RING, O_RDONLY);
    fstat(fd, &st);
    buf_size = (size_t)st.st_size;
    buf = mmap(NULL, buf_size, PROT_READ, MAP_SHARED, fd, 0);
    tail = rd32(0x10);
    valid = rd32(0x04);
    printf("tail=%u valid=%u\n", tail, valid);
    left = (int)valid;
    {
        uint32_t off = tail;
        while (left > 26) {
            uint8_t h[26];
            uint32_t len;
            uint16_t type;
            ring_read(off, h, 26);
            memcpy(&len, h, 4);
            memcpy(&type, h + 20, 2);
            if (type == 0x0401 || type == 0x0404 || type == 0x0422) {
                if (chain_shown < 3) {
                    uint8_t pl[1024];
                    uint32_t i, n = len;
                    if (n > sizeof(pl))
                        n = sizeof(pl);
                    ring_read((uint32_t)(off + 26), pl, n);
                    printf("type=0x%04x len=%u:", type, len);
                    for (i = 0; i < n && i < 160; i++)
                        printf(" %02x", pl[i]);
                    printf("\n");
                    chain_shown++;
                }
            } else if (type == 0x0400 && len <= 48 * 1024) {
                uint8_t pl[48 * 1024];
                uint32_t i;
                ring_read((uint32_t)(off + 26), pl, len);
                n0400++;
                for (i = 0; i + 4 < len; i++) {
                    if (pl[i] == 0 && pl[i + 1] == 0 && pl[i + 2] == 0 &&
                        pl[i + 3] == 1 && i + 4 < len)
                        ntl_hist[pl[i + 4] & 0x1F]++;
                }
            }
            off = (off + 26 + len) % DATA_SZ;
            left -= (int)(26 + len);
        }
    }
    {
        int i;
        printf("0x0400 payloads=%u, NAL headers found:", n0400);
        for (i = 0; i < 32; i++)
            if (ntl_hist[i])
                printf(" nt%d=%u", i, ntl_hist[i]);
        printf("\n");
    }
    return 0;
}
