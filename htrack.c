#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>

struct {
    struct sockaddr_storage addr;
    int timeout;
    int retry;
    int port;
    int count;
    int idle;
    int interval;
    int oneshot;
} ht = {
    .timeout = 5000,
    .retry = 3,
    .port = 80,
};

static int
ht_opt_flag(int argc, char **argv, void *data)
{
    int res = 1;

    memcpy(data, &res, sizeof(res));

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
ht_opt_int(int argc, char **argv, void *data)
{
    if (argc <= 1)
        return 1;

    errno = 0;

    char *end = NULL;
    long val = strtol(argv[1], &end, 0);
    int res = val;

    if (errno || argv[1] == end || val != (long)res)
        return 1;

    memcpy(data, &res, sizeof(res));

    return 0;
}

static int
ht_opt_host(int argc, char **argv, void *data)
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

/*
static int
ht_opt_str(int argc, char **argv, void *data)
{
    if (argc <= 1)
        return 1;

    memcpy(data, argv + 1, sizeof(char *));

    return 0;
}
*/

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

    if (ht.idle > 0 &&
        ht_sso(fd, IPPROTO_TCP, TCP_KEEPIDLE, ht.idle))
        return 1;

    if (ht.interval > 0 &&
        ht_sso(fd, IPPROTO_TCP, TCP_KEEPINTVL, ht.interval))
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
        {"host", ht_opt_host, &ht.addr},
        {"port", ht_opt_ushort, &ht.port},
        {"timeout", ht_opt_int, &ht.timeout},
        {"retry", ht_opt_int, &ht.retry},
        {"oneshot", ht_opt_flag, &ht.oneshot},
    };

    for (int i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
        if (ht_opt(argc, argv, opts[i].name, opts[i].f, opts[i].data)) {
            fprintf(stderr, "error: couldn't extract value for option `%s'\n", opts[i].name);
            return 1;
        }
    }

    switch (ht.addr.ss_family) {
    case AF_INET:
        ((struct sockaddr_in *)&ht.addr)->sin_port = htons(ht.port);
        break;
    case AF_INET6:
        ((struct sockaddr_in6 *)&ht.addr)->sin6_port = htons(ht.port);
        break;
    default:
        fprintf(stderr, "the option `host` is mandatory\n");
        return 1;
    }

    return 0;
}

int
main(int argc, char **argv)
{
    if (ht_init(argc, argv))
        return 1;

    struct pollfd pollfd = {
        .fd = socket(ht.addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK, 0),
        .events = POLLOUT,
    };

    if (pollfd.fd == -1) {
        perror("socket");
        return 1;
    }

    if (ht_keepalive(pollfd.fd))
        fprintf(stderr, "couldn't setup keepalive\n");

    if (ht_fastopen(pollfd.fd))
        fprintf(stderr, "couldn't setup fastopen\n");

    int ret = connect(pollfd.fd, (struct sockaddr *)&ht.addr, sizeof(ht.addr));

    if (ret == 0)
        return 0;

    if (ret == -1 && errno != EINPROGRESS) {
        perror("connect");
        return 1;
    }

    ret = poll(&pollfd, 1, ht.timeout);

    if (ret == 0) {
        ret = -1;
        errno = ETIMEDOUT;
    }

    if (ret == -1) {
        perror("poll");
        return 1;
    }

    if (pollfd.revents & POLLHUP) {
        fprintf(stderr, "pollhup");
        return 0;
    }

    if (!(pollfd.revents & POLLOUT))
        return 1;

    ret = 0;
    socklen_t retlen = sizeof(ret);

    if (getsockopt(pollfd.fd, SOL_SOCKET, SO_ERROR, &ret, &retlen) == -1) {
        perror("getsockopt");
        return 1;
    }

    if (ret) {
        errno = ret;
        perror("connect");
        return 1;
    }

    if (ht.oneshot)
        return 0;

    pollfd.events = 0;

    while (1) {
        ret = poll(&pollfd, 1, -1);

        if (ret == -1) {
            perror("poll");
            return 1;
        }

        if (pollfd.revents & POLLHUP) {
            fprintf(stderr, "pollhup");
            return 0;
        }
    }

    return 1;
}
