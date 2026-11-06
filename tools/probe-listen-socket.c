/* probe-listen-socket.c — connect to ecoengine's vfio-pci-sanway listen-socket
 * and report exactly what QEMU hands over: message bytes and SCM_RIGHTS fds.
 *
 * The vendor binary says "vfio: failed to send fd to client on %s", so this
 * socket is a fd-passing channel.  If the fds are dma-bufs of the M1000 vGPU
 * scanout, the viewer can mmap them and skip every guest-side copy.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>

static const char *fd_kind(int fd) {
    char p[64], b[256];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, b, sizeof b - 1);
    if (n < 0) return "?";
    b[n] = 0;
    static char out[300];
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
        snprintf(out, sizeof out, "%s (regular, %lld bytes)", b, (long long)st.st_size);
    else
        snprintf(out, sizeof out, "%s", b);
    return out;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/qemumdev.sock";
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("connect");
        return 1;
    }
    fprintf(stderr, "connected to %s\n", path);

    /* The vendor device is documented to send on connect; read a few messages
     * and every fd that rides along. */
    for (int round = 0; round < 4; round++) {
        char buf[512];
        char cbuf[CMSG_SPACE(sizeof(int) * 16)];
        struct iovec iov = { buf, sizeof buf };
        struct msghdr mh = {0};
        mh.msg_iov = &iov; mh.msg_iovlen = 1;
        mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
        ssize_t n = recvmsg(s, &mh, 0);
        if (n < 0) { perror("recvmsg"); break; }
        if (n == 0) { fprintf(stderr, "peer closed\n"); break; }
        printf("--- msg %d: %zd data bytes:", round, n);
        for (ssize_t i = 0; i < n && i < 32; i++) printf(" %02x", (unsigned char)buf[i]);
        printf("\n");
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            int *fds = (int *)CMSG_DATA(c);
            for (int i = 0; i < cnt; i++) {
                printf("    fd[%d] = %d  %s\n", i, fds[i], fd_kind(fds[i]));
                /* a dma-buf answers this ioctl; a plain file does not */
                struct dma_buf_info { int dummy; } di;
                if (ioctl(fds[i], DMA_BUF_IOCTL_SYNC, &(struct dma_buf_sync){0}) == 0)
                    printf("        (accepts DMA_BUF_IOCTL_SYNC -> dma-buf)\n");
            }
        }
    }
    return 0;
}
