#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>

#include "argz/argz.h"

#ifndef TCP_FASTOPEN
#define TCP_FASTOPEN 23
#endif

struct {
    struct sockaddr_storage remote, local;
    unsigned short port;
    int count;
    int timeout;
    struct {
        unsigned char *data;
        size_t size;
    } req, buf;
} ht = {
    .port = 80,
    .count = 3,
    .timeout = 5,
    .buf = {
        .size = 4096,
    },
};

static int
ht_sso(int fd, int level, int option, int val)
{
    return setsockopt(fd, level, option, &val, sizeof(val));
}

static int
ht_keepalive(int fd)
{
    if (ht_sso(fd, SOL_SOCKET, SO_KEEPALIVE, 1))
        return 1;

    if (ht.count > 0 &&
        ht_sso(fd, IPPROTO_TCP, TCP_KEEPCNT, ht.count))
        return 1;

    if (ht.timeout > 0 &&
        (ht_sso(fd, IPPROTO_TCP, TCP_KEEPIDLE, ht.timeout) ||
         ht_sso(fd, IPPROTO_TCP, TCP_KEEPINTVL, ht.timeout)))
        return 1;

    return 0;
}

static int
ht_fastopen(int fd)
{
    if (ht_sso(fd, IPPROTO_TCP, TCP_FASTOPEN, 1))
        return 1;

    return 0;
}

static void
ht_setup_port(struct sockaddr_storage *ss, uint16_t port)
{
    switch (ss->ss_family) {
    case AF_INET:
        ((struct sockaddr_in *)ss)->sin_port = htons(port);
        break;
    case AF_INET6:
        ((struct sockaddr_in6 *)ss)->sin6_port = htons(port);
        break;
    }
}

static int
ht_init(int argc, char **argv)
{
    unsigned count = ht.count;
    unsigned timeout = 1000U * ht.timeout;

    struct argz htz[] = {
        {NULL, "IPADDR", &ht.remote, argz_addr},
        {NULL, "PORT", &ht.port, argz_ushort},
        {"bind", "IPADDR", &ht.local, argz_addr},
        {"send", "TEXT", &ht.req.data, argz_str},
        {"timeout", "SECONDS", &timeout, argz_time},
        {"count", "COUNT", &count, argz_ulong},
        {"bufsize", "BYTES", &ht.buf.size, argz_bytes},
        {NULL},
    };

    if (argz(htz, argc, argv))
        return 1;

    if (!ht.remote.ss_family) {
        fprintf(stderr, "option `host' is mandatory\n");
        return 1;
    }

    if (count > INT_MAX) {
        fprintf(stderr, "option `count' is too big\n");
        return 1;
    }

    timeout /= 1000U;

    if (timeout > INT_MAX) {
        fprintf(stderr, "option `timeout' is too big\n");
        return 1;
    }

    ht.count = (int)count;
    ht.timeout = (int)timeout;

    ht_setup_port(&ht.remote, ht.port);

    if (ht.local.ss_family &&
        ht.local.ss_family != ht.remote.ss_family) {
        fprintf(stderr, "host and bind are not compatible\n");
        return 1;
    }

    ht.buf.data = malloc(ht.buf.size);

    if (!ht.buf.data) {
        perror("malloc");
        return 1;
    }

    if (ht.req.data)
        ht.req.size = strlen((const char *)ht.req.data);

    return 0;
}

static int
ht_send(int fd, unsigned char *data, size_t size)
{
    size_t sent = 0;

    while (sent < size) {
        ssize_t r = send(fd, data + sent, size - sent, 0);

        if (r == (ssize_t)-1) {
            if (errno == EAGAIN)
                continue;

            perror("send");
            return 1;
        }

        sent += r;
    }

    return 0;
}

static int
ht_addrlen(struct sockaddr_storage *ss)
{
    return (ss->ss_family == AF_INET) ? sizeof(struct sockaddr_in)
                                      : sizeof(struct sockaddr_in6);
}

static int
ht_bind(int fd)
{
    if (!ht.local.ss_family)
        return 0;

    int ret = bind(fd, (struct sockaddr *)&ht.local, ht_addrlen(&ht.local));

    if (ret == -1) {
        perror("bind");
        return 1;
    }

    return 0;
}

static int
ht_connect(int fd)
{
    int ret = connect(fd, (struct sockaddr *)&ht.remote, ht_addrlen(&ht.remote));

    if (ret == -1 && errno != EINPROGRESS) {
        perror("connect");
        return 1;
    }

    struct pollfd pollfd = {
        .fd = fd,
        .events = POLLOUT,
    };

    ret = poll(&pollfd, 1, ht.timeout * 1000);

    if (ret == 0)
        errno = ETIMEDOUT;

    if (ret <= 0) {
        perror("poll");
        return 1;
    }

    if (!(pollfd.revents & POLLOUT))
        return 1;

    ret = 0;
    socklen_t retlen = sizeof(ret);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &ret, &retlen) == -1) {
        perror("getsockopt");
        return 1;
    }

    if (ret) {
        errno = ret;
        perror("connect");
        return 1;
    }

    return 0;
}

static int
ht_wait(int fd)
{
    struct pollfd pollfd = {
        .fd = fd,
        .events = POLLIN,
    };

    while (1) {
        int ret = poll(&pollfd, 1, -1);

        if (ret == -1) {
            perror("poll");
            return 1;
        }

        if (pollfd.revents & POLLIN) {
            ssize_t r = read(fd, ht.buf.data, ht.buf.size);

            if (r == (ssize_t)-1) {
                perror("read");
                return 1;
            }

            if (!r)
                break;
        }
    }

    return 0;
}

static int
ht_socket(void)
{
    int fd = socket(ht.remote.ss_family, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);

    if (fd == -1) {
        perror("socket");
        return -1;
    }

    if (ht_keepalive(fd))
        fprintf(stderr, "couldn't setup keepalive\n");

    if (ht_fastopen(fd))
        fprintf(stderr, "couldn't setup fastopen\n");

    return fd;
}

int
main(int argc, char **argv)
{
    if (ht_init(argc, argv))
        return 1;

    while (1) {
        int fd = ht_socket();

        if (fd < 0)
            return 1;

        if (ht_bind(fd) ||
            ht_connect(fd) ||
            ht_send(fd, ht.req.data, ht.req.size) ||
            ht_wait(fd))
            break;

        if (fd >= 0)
            close(fd);

        sleep(ht.timeout);
    }

    return 0;
}
