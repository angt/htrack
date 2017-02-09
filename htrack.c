#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>

#ifndef TCP_FASTOPEN
#define TCP_FASTOPEN 23
#endif

struct {
    struct sockaddr_storage remote, local;
    int port;
    int count;
    int timeout;
    struct {
        char *data;
        int size;
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
ht_opt_str(int argc, char **argv, void *data)
{
    if (argc <= 1)
        return 1;

    memcpy(data, argv + 1, sizeof(char *));

    return 0;
}

static int
ht_opt_ushort(int argc, char **argv, void *data)
{
    if (argc <= 1)
        return 1;

    errno = 0;

    char *end = NULL;
    long val = strtol(argv[1], &end, 0);
    unsigned short res = val;

    if (errno || argv[1] == end || val != (long)res)
        return 1;

    memcpy(data, &res, sizeof(res));

    return 0;
}

static int
ht_opt_nat(int argc, char **argv, void *data)
{
    if (argc <= 1)
        return 1;

    errno = 0;

    char *end = NULL;
    long val = strtol(argv[1], &end, 0);
    int res = val;

    if (errno || argv[1] == end || val < 0 || val != (long)res)
        return 1;

    memcpy(data, &res, sizeof(res));

    return 0;
}

static int
ht_opt_addr(int argc, char **argv, void *data)
{
    if (argc <= 1)
        return 1;

    struct sockaddr_storage addr;
    struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&addr;

    memcpy(&addr, data, sizeof(addr));

    if (inet_pton(AF_INET, argv[1], &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET,
        memcpy(data, &addr, sizeof(addr));
        return 0;
    }

    if (inet_pton(AF_INET6, argv[1], &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6,
        memcpy(data, &addr, sizeof(addr));
        return 0;
    }

    return -1;
}

static int
ht_opt(int argc, char **argv, char *name,
       int (*f)(int, char **, void *), void *data)
{
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], name))
            return f(argc - i, argv + i, data);

    return 0;
}

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

static int
ht_init(int argc, char **argv)
{
    struct {
        char *name;
        int (*f)(int, char **, void *);
        void *data;
    } opts[] = {
        {"host", ht_opt_addr, &ht.remote},
        {"port", ht_opt_ushort, &ht.port},
        {"bind", ht_opt_addr, &ht.local},
        {"send", ht_opt_str, &ht.req.data},
        {"timeout", ht_opt_nat, &ht.timeout},
        {"count", ht_opt_nat, &ht.count},
        {"bufsize", ht_opt_nat, &ht.buf.size},
    };

    for (int i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
        if (ht_opt(argc, argv, opts[i].name, opts[i].f, opts[i].data)) {
            fprintf(stderr, "bad value for option `%s'\n", opts[i].name);
            return 1;
        }
    }

    in_port_t port = htons(ht.port);

    switch (ht.remote.ss_family) {
    case AF_INET:
        memcpy(&((struct sockaddr_in *)&ht.remote)->sin_port,
               &port, sizeof(port));
        break;
    case AF_INET6:
        memcpy(&((struct sockaddr_in6 *)&ht.remote)->sin6_port,
               &port, sizeof(port));
        break;
    default:
        fprintf(stderr, "option `host' is mandatory\n");
        return 1;
    }

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
        ht.req.size = strlen(ht.req.data);

    return 0;
}

static int
ht_send(int fd, char *data, int size)
{
    int sent = 0;

    while (sent < size) {
        int r = send(fd, data + sent, size - sent, 0);

        if (r == -1) {
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
            int r = read(fd, ht.buf.data, ht.buf.size);

            if (r == -1) {
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
