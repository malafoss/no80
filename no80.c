#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdnoreturn.h>

#define QUEUE_LENGTH 1000

enum command { REDIRECT, PERMADIRECT};

/* signal handler */
noreturn void interrupted(int sig)
{
    puts("interrupted - exiting.");
    _exit(0); /* handler safe exit closes open sockets */
}

/* returns responseSize and puts http response buffer into given response pointer */
int build_response(enum command cmd, const char *url, char **response)
{
    const char *template = NULL;
    if (cmd == REDIRECT) {
        template = "HTTP/1.1 302 Found\r\n"
                   "Location: %s\r\n"
                   "Server: no80\r\n"
                   "Connection: close\r\n"
                   "\r\n";
    } else if (cmd == PERMADIRECT) {
        template = "HTTP/1.1 301 Moved Permanently\r\n"
                   "Location: %s\r\n"
                   "Server: no80\r\n"
                   "Connection: close\r\n"
                   "\r\n";
    }
    int responseMax = strlen(template) + strlen(url); /* note: template has 2 extra characters */
    *response = malloc(responseMax+1); /* allocated once, kernel will free */
    int responseSize = snprintf(*response, responseMax, template, url);
    if (responseSize < 0) {
        perror("snprintf");
        exit(2);
    }
    return responseSize;
}

/* returns listen socket fd */
int listen_socket(int port)
{
    /* prepare socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        exit(2);
    }

    /* enable address reusage */
    int option = 1; /* enable */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option))) {
        perror("setsockopt");
        exit(2);
    }

    /* enable deferred accept */
    option = 3; /* seconds */
    if (setsockopt(fd, SOL_TCP, TCP_DEFER_ACCEPT, &option, sizeof(option))) {
        perror("setsockopt");
        exit(2);
    }

    /* bind the port */
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&address, sizeof(address)) == -1) {
        perror("bind");
        exit(2);
    }

    /* list the port */
    if (listen(fd, QUEUE_LENGTH) == -1) {
        perror("listen");
        exit(2);
    }

    return fd;
}

noreturn void server(int port, enum command cmd, const char *url)
{
    /* build http response */
    char *response;
    int responseSize = build_response(cmd, url, &response);

    /* prepare listen socket */
    int fd = listen_socket(port);

    /* serve incoming connections */
    while (1) {
        /* accept connection */
        int rfd = accept(fd, NULL, NULL);
        if (rfd == -1) {
            /* retry accept for these errnos */
            switch (errno) {
            case EAGAIN:
            case EINPROGRESS:
            case ENETDOWN:
            case EPROTO:
            case ENOPROTOOPT:
            case EHOSTDOWN:
            case ENONET:
            case EHOSTUNREACH:
            case EOPNOTSUPP:
            case ENETUNREACH:
                continue;
            }
            perror("accept");
            exit(2);
        }

        /* not going to even recv what browser has to say */

        /* send response */
        send(rfd, response, responseSize, 0);

        /* tear down */
        close(rfd);
    }
}

int main(int argc, char **argv)
{
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);

    if (argc != 3) {
        puts("Usage: no80 redirect URL");
        puts("       no80 permadirect URL");
        return 1;
    }

    const char *cmdstr = argv[1];
    const char *url = argv[2];
    int port = 80;

    enum command cmd;
    if (strcmp(cmdstr, "redirect") == 0) {
        cmd = REDIRECT;
    } else if (strcmp(cmdstr, "permadirect") == 0) {
        cmd = PERMADIRECT;
    } else {
        puts("Unknown command");
        return 1;
    }

    puts("no80 - the resource effective redirecting http server");
    printf("%sing port %d to %s\n", cmdstr, port, url);
    server(port, cmd, url);
}
