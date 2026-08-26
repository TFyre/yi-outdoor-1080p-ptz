/* typescan.c - walk the record ring with the CORRECT framing (26-byte
 * header: len@+0, seq@+4, magic@+8, ts@+16, type@+20) from the
 * published tail for the published valid bytes, and report the type
 * histogram + the NAL types inside the hi-res (0x0400) payloads.
 * Answers: does this era emit IDR (nt=5) records at all, and where?
 *
 * Run on the camera:  typescan
 */
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
    unsigned types[65536];
    unsigned nt_hist[32];
    unsigned chain_pos[16];
    int nchain = 0;
    uint32_t last_chain_seq = 0;
    int recs_since_chain = 0;
    int idr_since_chain[16];
    int nidr = 0;
    int i;

    memset(types, 0, sizeof(types));
    memset(nt_hist, 0, sizeof(nt_hist));
    memset(idr_since_chain, 0, sizeof(idr_since_chain));

    fd = open(RING, O_RDONLY);
    if (fd < 0) { perror(RING); return 1; }
    fstat(fd, &st);
    buf_size = (size_t)st.st_size;
    buf = mmap(NULL, buf_size, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 1; }

    tail = rd32(0x10);
    valid = rd32(0x04);
    printf("tail=%u valid=%u seq=%u\n", tail, valid, rd32(0x18));
    left = (int)valid;
    {
        uint32_t off = tail;
        while (left > 26) {
            uint8_t h[26];
            uint32_t len, seq;
            uint16_t type;
            ring_read(off, h, 26);
            memcpy(&len, h, 4);
            memcpy(&seq, h + 4, 4);
            memcpy(&type, h + 20, 2);
            if (len > DATA_SZ || len + 26 > (uint32_t)left) {
                printf("BAD RECORD at off=%u len=%u left=%d\n", off, len, left);
                break;
            }
            types[type]++;
            if (type == 0x0422) {
                chain_pos[nchain % 16] = off;
                if (nchain < 16) {
                    idr_since_chain[nchain] = 0;
                } else {
                    idr_since_chain[15] = 0;
                }
                last_chain_seq = seq;
                recs_since_chain = 0;
                nchain++;
            }
            if (type == 0x0400 && len >= 5 && len <= 48 * 1024) {
                /* locate the SC in the payload, read the NAL header */
                uint8_t pl[48 * 1024];
                uint32_t k = 0;
                if (len <= sizeof(pl)) {
                    ring_read((uint32_t)(off + 26), pl, len);
                    while (k + 4 <= len &&
                           !(pl[k] == 0 && pl[k + 1] == 0 &&
                             pl[k + 2] == 0 && pl[k + 3] == 1))
                        k++;
                    if (k + 4 <= len)
                        nt_hist[pl[k + 4] & 0x1F]++;
                    else
                        nt_hist[31]++;   /* no SC in the payload */
                    if (k + 4 <= len && (pl[k + 4] & 0x1F) == 5) {
                        nidr++;
                        if (nchain > 0) {
                            if (nchain <= 16)
                                idr_since_chain[nchain - 1]++;
                            else
                                idr_since_chain[15]++;
                        }
                    }
                }
            }
            if (type & 0x0400)
                recs_since_chain++;
            off = (off + 26 + len) % DATA_SZ;
            left -= (int)(26 + len);
        }
        printf("walked %d bytes, left=%d\n", (int)valid - left, left);
    }
    printf("types:\n");
    for (i = 0; i < 65536; i++)
        if (types[i])
            printf("  0x%04x: %u\n", i, types[i]);
    printf("0x0400 NAL types: ");
    for (i = 0; i < 32; i++)
        if (nt_hist[i])
            printf("nt%d=%u ", i, nt_hist[i]);
    printf("\nchains=%d idrs=%d\n", nchain, nidr);
    return 0;
}
