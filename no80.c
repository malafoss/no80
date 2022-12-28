#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <assert.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdnoreturn.h>
#include <sys/epoll.h>

static_assert(EAGAIN == EWOULDBLOCK);

#define STR(s) #s
#define STR_EVAL(e) STR(e)
#define VERSION_STR STR_EVAL(VERSION)

/* TCP parameters */
#define QUEUE_LENGTH 1000

/* Read buffer size */
#define BUFFER_SIZE 8192

/* Max HTTP protocol element sizes */
#define MAX_METHOD 10
#define MAX_PATH 8000
static_assert(BUFFER_SIZE > MAX_METHOD + MAX_PATH + 128);

/* Epoll parameters */
#define MAX_EPOLL_CREATES 256
#define MAX_EPOLL_EVENTS 256
#define EPOLL_TIMEOUT_MS 60*1000

/* redirecting command mode */
enum command { REDIRECT = 0, PERMADIRECT = 1 };

/* maximum number of matched path redirections */
#define MAX_MATCHES 256

/* globals for statistics */
bool noStatistics = 0;
time_t startTime;
unsigned long requests = 0;
unsigned long successes = 0;
unsigned long completed = 0;
int connections = -1; /* ignore efd */
int maxConns = 0;
int maxEvents = 0;

/* signal handler SIGINTR and SIGTERM */
noreturn void interrupted(int sig)
{
    _exit(0); /* handler safe exit closes open sockets */
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
    {
        int option = 1; /* enable */
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option))) {
            perror("setsockopt");
            exit(2);
        }
    }

    /* enable immediate send */
    {
        int option = 1; /* enable */
        if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &option, sizeof(option))) {
            perror("setsockopt");
            exit(2);
        }
    }

    /* bind the port */
    {
        struct sockaddr_in address;
        bzero(&address, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);
        if (bind(fd, (struct sockaddr*)&address, sizeof(address)) == -1) {
            perror("bind");
            exit(2);
        }
    }

    /* list the port */
    if (listen(fd, QUEUE_LENGTH) == -1) {
        perror("listen");
        exit(2);
    }

    return fd;
}

/* match path and target url pairs */
static struct match_params {
    const char *path; /* matched path */
    int pathSize;
    const char *url; /* target url */
    int urlSize;
    bool append; /* append path to url */
    bool begin; /* match the beginning of the path only */
} pathMatch[MAX_MATCHES];

/* shared read-only server context for all threads */
static struct server_params {
    enum command cmd; /* server command */
    bool append; /* append path to url */
    struct match_params *pathMatch; /* path matches array */
    int matches; /* number of path matches in the array */

    /* default output parameters */
    const char *header; /* header to output */
    int headerSize;
    const char *url; /* url to redirect to */
    int urlSize;
    const char *tailer; /* tailer to output */
    int tailerSize;
} serverContext;

/* connection related parameters */
struct connect_params {
    int fd;
    struct epoll_event ee;

    /* read params */
    char *recvPtr;
    const char *methodBegin;
    const char *methodEnd;
    const char *pathBegin;
    const char *pathEnd;
    char buffer[BUFFER_SIZE];

    /* target url */
    const char *url;
    int urlSize;

    /* send params */
    int headerSent;
    int urlSent;
    int pathSent;
    int tailerSent;
};

/* try finding matching path, NULL if no match */
struct match_params *match_path(const char *p, int size)
{
    if (size <= 0) return NULL;
    struct match_params *match = &(serverContext.pathMatch[0]);
    int matches = serverContext.matches;
    while (matches--) {
        int msize = match->pathSize;
        /* if match begin then match the beginning of the path only */
        if (msize == size || (match->begin && msize < size))
            if (memcmp(p, match->path, msize) == 0)
                return match;
        match++;
    }
    return NULL;
}

/* read http request - 0 = OK, 1 = Call again, -1 = Error */
int read_request(struct connect_params *cp)
{
    int bufferSize = sizeof(cp->buffer);

    /* read request beginning in form: Method SP Request-URI SP */
    /* allowed Request-URI is in form: '/' PATH */

    if (!cp->recvPtr) cp->recvPtr = cp->buffer;
    if (!cp->methodBegin) cp->methodBegin = cp->buffer;

    char *ptr = cp->recvPtr;
    while (ptr < cp->buffer + bufferSize) {
        if (ptr == cp->recvPtr) {
            /* recv more data */
            int bytes = recv(cp->fd, cp->recvPtr, bufferSize - (cp->recvPtr - cp->buffer), MSG_DONTWAIT);
            if (bytes == -1) {
                if (errno == EAGAIN) return 1; // recv more later
                perror("recv");
                return -1;
            }
            if (bytes == 0) return -1; // needed bytes, so its error
            cp->recvPtr += bytes;
        }

        char c = *ptr++;

        /* stop if disallowed characters */
        if (c == '\r' || c == '\n') break;

        if (!cp->methodEnd) {
            /* in Method */
            if (ptr - cp->buffer > MAX_METHOD) break;
            if (c == ' ') {
                cp->methodEnd = ptr-1;
                cp->pathBegin = ptr;
            }
        } else {
            /* in Request-URI */
            if (ptr - cp->pathBegin > MAX_PATH || c == ' ') {
                cp->pathEnd = ptr-1;
                break;
            }
        }
    }

    /* ignore rest of the data */
    recv(cp->fd, cp->buffer, bufferSize, MSG_DONTWAIT|MSG_TRUNC);

    if (cp->pathBegin && cp->pathEnd && *(cp->pathBegin) == '/') {
        /* path found */

        /* try matching path */
        struct match_params *match = match_path(cp->pathBegin, cp->pathEnd - cp->pathBegin);
        if (match) {
            /* match found, redirect to matching url */
            cp->url = match->url;
            cp->urlSize = match->urlSize;

            if (match->append) {
                /* match with append */

                /* append non-matching part of the path to the matching url */
                cp->pathBegin += match->pathSize;
                if (cp->pathBegin > cp->pathEnd) cp->pathBegin = cp->pathEnd;
                return 0;
            }

            /* match without append */
            cp->pathBegin = cp->buffer;
            cp->pathEnd = cp->buffer;
            return 0;
        }

        /* default match, redirect to default url */
        cp->url = serverContext.url;
        cp->urlSize = serverContext.urlSize;

        if (serverContext.append) {
            /* default match with append */
            return 0;
        }

        /* default match without append */
        cp->pathBegin = cp->buffer;
        cp->pathEnd = cp->buffer;
        return 0;
    }

    /* no path - default match - return default url with empty path */
    cp->url = serverContext.url;
    cp->urlSize = serverContext.urlSize;
    cp->pathBegin = cp->buffer;
    cp->pathEnd = cp->buffer;
    return 0;
}

/* returns the first part of the response */
const char *get_header(enum command cmd)
{
    if (cmd == REDIRECT) {
        return "HTTP/1.1 302 Found\r\n"
               "Location: ";
    } else if (cmd == PERMADIRECT) {
        return "HTTP/1.1 301 Moved Permanently\r\n"
               "Location: ";
    }
    return NULL;
}

/* returns the last part of the response */
const char *get_tailer()
{
    return "\r\n"
           "Server: no80/" VERSION_STR "\r\n"
           "Connection: close\r\n"
           "\r\n";
}

/* print statistics */
void print_stats()
{
    if (!noStatistics) {
        printf("+%lus: %lu requests (%lu successes, %lu failures, %d ongoing) (%d max events, %d max conns)\n",
            time(NULL) - startTime,
            requests,
            successes,
            completed - successes,
            connections,
            maxEvents,
            maxConns);
    }
    maxEvents = 0;
    maxConns = 0;
}

/* allocate new connection parameters */
struct connect_params *new_connect(int fd)
{
    struct connect_params *c = malloc(sizeof(struct connect_params));
    if (!c) return NULL;
    bzero(c, sizeof(struct connect_params));
    c->fd = fd;
    ++connections;
    if (connections > maxConns) maxConns = connections;
    return c;
}

int send_part(int fd, const char *msg, int msgsize, int *sent, bool last)
{
    int rc = send(fd, msg + *sent, msgsize - *sent, MSG_DONTWAIT | (last ? 0 : MSG_MORE));
    if (rc < 0) {
        if (errno == EAGAIN) return 1;
        perror("send");
        return -1;
    }
    *sent += rc;
    return 0;
}

/* send http response - 0 = OK, 1 = Call again, -1 = Error */
int send_response(struct connect_params *cp)
{
    int rc;

    /* send response */
    if (serverContext.headerSize > cp->headerSent) {
        rc = send_part(cp->fd, serverContext.header, serverContext.headerSize, &(cp->headerSent), 0);
        if (rc != 0) return rc;
    }

    if (cp->urlSize > cp->urlSent) {
        rc = send_part(cp->fd, cp->url, cp->urlSize, &(cp->urlSent), 0);
        if (rc != 0) return rc;
    }

    int pathSize = cp->pathEnd - cp->pathBegin;
    if (pathSize > cp->pathSent) {
        rc = send_part(cp->fd, cp->pathBegin, pathSize, &(cp->pathSent), 0);
        if (rc != 0) return rc;
    }

    if (serverContext.tailerSize > cp->tailerSent) {
        rc = send_part(cp->fd, serverContext.tailer, serverContext.tailerSize, &(cp->tailerSent), 1);
        if (rc != 0) return rc;
    }

    /* tear down */
    shutdown(cp->fd, SHUT_RDWR);
    return 0;
}

/* free allocated connection parameters */
void free_connect(struct connect_params *cp)
{
    if (cp) {
        close(cp->fd);
        free(cp);
    }
    --connections;
    ++completed;
}

/* http server */
noreturn void server(int port, enum command cmd, const char *url, bool append, struct match_params *pathMatch, int matches)
{
    /* prepare shared read-only server context for all threads */
    serverContext.cmd = cmd;
    serverContext.append = append;
    serverContext.header = get_header(cmd);
    serverContext.headerSize = strlen(serverContext.header);
    serverContext.url = url;
    serverContext.urlSize = strlen(url);
    serverContext.tailer = get_tailer();
    serverContext.tailerSize = strlen(serverContext.tailer);
    serverContext.pathMatch = pathMatch;
    serverContext.matches = matches;

    startTime = time(NULL);

    /* prepare listen socket */
    const int fd = listen_socket(port);

    const int efd = epoll_create(MAX_EPOLL_CREATES);
    if (efd == -1) {
        perror("epoll_create");
        exit(2);
    }

    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = new_connect(fd);
    if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ee) < 0) {
        perror("epoll_ctl");
        exit(2);
    }

    struct epoll_event ees[MAX_EPOLL_EVENTS];

    /* serve incoming events */
    while (1) {
        int count = epoll_wait(efd, ees, sizeof(ees)/sizeof(ees[0]), EPOLL_TIMEOUT_MS);
        if (count == -1) {
            perror("epoll_wait");
            exit(2);
        }
        if (count == 0) {
            /* timeout */
            if (maxEvents > 0) {
                print_stats();
            }
            continue;
        }
        if (count > maxEvents) maxEvents = count;

        for (int i = 0; i < count; i++) {
            struct connect_params *cp = (struct connect_params *)ees[i].data.ptr;

            if (cp->fd == fd) {
                /* accept new connection event */
                const int rfd = accept(fd, NULL, NULL);
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
                    case EMFILE:
                    case ENFILE:
                        /* out of file descriptors */
                        /* adjust ulimit accordingly */
                        perror("accept");
                        continue;
                    default:
                        perror("accept");
                        exit(2);
                    }
                }

                /* nonblocking socket */
                fcntl(rfd, F_SETFL, fcntl(rfd, F_GETFL) | O_NONBLOCK);

                /* new connection -> EPOLLIN event */
                struct connect_params *cp = new_connect(rfd);
                cp->ee.events = EPOLLIN;
                cp->ee.data.ptr = cp;
                if (epoll_ctl(efd, EPOLL_CTL_ADD, rfd, &(cp->ee)) < 0) {
                    perror("epoll_ctl add");
                    free_connect(cp);
                    continue;
                }
                ++requests;
                if ((requests % 1000) == 0) print_stats();
                continue;
            }

            /* handle connection events */
            if (ees[i].events & EPOLLIN) {
                /* read request */
                int rc = read_request(cp);
                if (rc < 0) {
                    /* failure -> delete event */
                    if (epoll_ctl(efd, EPOLL_CTL_DEL, cp->fd, NULL) < 0) {
                        perror("epoll_ctl del");
                    }
                    free_connect(cp);
                    continue;
                }
                if (rc > 0) {
                    /* recv more when available */
                    continue;
                }
                /* success -> EPOLLOUT event */
                cp->ee.events = EPOLLOUT;
                if (epoll_ctl(efd, EPOLL_CTL_MOD, cp->fd, &(cp->ee)) < 0) {
                    perror("epoll_ctl out");
                }
                continue;
            }
            if (ees[i].events & EPOLLOUT) {
                /* send response -> delete event */
                int rc = send_response(cp);
                if (rc > 0) {
                    /* send more again later */
                    continue;
                }
                if (rc == 0) ++successes;
                if (epoll_ctl(efd, EPOLL_CTL_DEL, cp->fd, NULL) < 0) {
                    perror("epoll_ctl del");
                }
                free_connect(cp);
                continue;
            }
            /* unknown event */
        }
    }
}

/* print help text */
void printHelp()
{
    puts("Usage: no80 [OPTION]... URL\n"
         "\n"
         "The resource effective redirecting http server v" VERSION_STR "\n"
         "\n"
         "Options:\n"
         "  -a           Append path from the http request to the redirected URL\n"
         "  -h           Print this help text and exit\n"
         "  -m PATH URL  Redirect path matching with PATH to URL\n"
         "  -s PATH URL  Redirect path starting with PATH to URL\n"
         "  -r PATH URL  Redirect path starting with PATH to URL appended with the rest of the path\n"
         "  -p N         Use specified port number N (default is port 80)\n"
         "  -P           Redirect permanently using 301 instead of temporarily using 302\n"
         "  -q           Suppress statistics");
}

int main(int argc, char **argv)
{
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);

    if (argc < 2) {
        printHelp();
        return 1;
    }

    bool option_P = 0;
    bool option_a = 0;
    int port = 80;
    const char *url = NULL;
    int matches = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-P") == 0) {
            option_P = 1;
        } else if (strcmp(argv[i], "-a") == 0) {
            option_a = 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            noStatistics = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            if (++i < argc) {
                port = atoi(argv[i]);
                if (port < 1 || port > 65535) {
                    puts("Invalid port number specified");
                    return 1;
                }
            } else {
                puts("Missing port number");
                return 1;
            }
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-r") == 0) {
            bool matchAppend = (argv[i][1] == 'r');
            bool matchBegin = (argv[i][1] != 'm');
            if (++i < argc) {
                const char *m = argv[i];
                if (++i < argc) {
                    const char *u = argv[i];
                    if (matches < MAX_MATCHES - 1) {
                        pathMatch[matches].path = m;
                        pathMatch[matches].pathSize = strlen(m);
                        pathMatch[matches].url = u;
                        pathMatch[matches].urlSize = strlen(u);
                        pathMatch[matches].append = matchAppend;
                        pathMatch[matches].begin = matchBegin;
                        ++matches;
                    } else {
                        puts("Too many match parameters");
                        return 1;
                    }
                } else {
                    puts("Missing match target url");
                    return 1;
                }
            } else {
                puts("Missing match path");
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printHelp();
            return 1;
       } else if (!url) {
            url = argv[i];
       } else {
            printf("Invalid parameter %s\n", argv[i]);
            return 1;
       }
    }
    if (!url) {
        puts("Missing URL parameter");
        return 1;
    }

    enum command cmd = (option_P ? PERMADIRECT : REDIRECT);
    bool append = (option_a ? 1 : 0);

    printf("no80 - The resource effective redirecting http server v" VERSION_STR "\n"
           "Redirecting port %d requests %s to url %s%s\n",
        port,
        ( option_P ? "permanently (301)" : "temporarily (302)"),
        url,
        ( option_a ? "</path>" : ""));

    server(port, cmd, url, append, pathMatch, matches);
}
