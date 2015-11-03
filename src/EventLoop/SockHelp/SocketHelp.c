#include "SocketHelp.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>

int tcp_connect(const char *host, unsigned short port) {

    struct addrinfo hints, *res, *ressave;

    bzero(&hints, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    char server[6] = {'\0'};
    sprintf(server, "%d", port);
    if (getaddrinfo(host, server, &hints, &res) != 0) {
        return -1;
    }

    ressave = res;
    int sock_fd;
    do {
        sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock_fd < 0) {
            continue;
        }

        if (connect(sock_fd, res->ai_addr, res->ai_addrlen) == 0) {
            break;
        }

        close(sock_fd);
    } while ((res = res->ai_next) != NULL);

    if (res == NULL) {
        return -1;
    }

    freeaddrinfo(ressave);
    return sock_fd;
}

int create_tcp_listen(unsigned short port, int reuse) {
    struct sockaddr_in addr;
    memset(&addr, 0x00, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    if (fcntl(socket_fd, F_SETFL, O_NONBLOCK)) {
        return -1;
    }

    int res;
    if (reuse != 0) {
        const int _reuse = 1;
        res = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &_reuse, sizeof(int));
        if (res < 0) {
            return -1;
        }
    }

    res = bind(socket_fd, (struct sockaddr *) &addr, sizeof(addr));
    if (res < 0) {
        return -1;
    }

    res = listen(socket_fd, 10);
    if (res < 0) {
        return -1;
    }
    return socket_fd;
}

int set_no_block(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}