#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <linux/sched.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdnoreturn.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define VERSION "0.1"

#define QUEUE_LENGTH 1000
#define BUFFER_SIZE 8192
#define THREADING

#define MAX_METHOD 10
#define MAX_PATH 8000

enum command { REDIRECT = 0, REDIRECT_HOST = 1, PERMADIRECT = 2, PERMADIRECT_HOST = 3 };

/* globals */
_Atomic int threadCount = 0;
int maxThreadCount = 0;
time_t startTime;

/* clone3 system call */
inline pid_t sys_clone3(struct clone_args *args, int argsSize)
{
	return syscall(__NR_clone3, args, argsSize);
}

/* fork thread using fork semantics */
inline pid_t fork_thread()
{
    struct clone_args ca;
    bzero(&ca, sizeof(ca));
    ca.flags = CLONE_FILES;
    ca.exit_signal = SIGCHLD;

    pid_t rc = sys_clone3(&ca, sizeof(ca));
    if (rc > 0) {
        /* parent */
        threadCount++;
        if (threadCount > maxThreadCount) maxThreadCount = threadCount;
    }
    return rc;
}

/* signal handler SIGINTR and SIGTERM */
noreturn void interrupted(int sig)
{
    puts("interrupted - exiting.");
    _exit(0); /* handler safe exit closes open sockets */
}

/* signal handler SIGCHLD */
void thread_exited(int sig)
{
    /* kill the zombies */
    int status;
    while (waitpid(-1, &status, WUNTRACED | WNOHANG) > 0) threadCount--;
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

    /* enable lingering close */
    {
        struct linger option;
	    bzero(&option, sizeof(option));
        option.l_onoff = 1;
	    option.l_linger = 1; /* seconds */
        if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &option, sizeof(option))) {
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

/* shared read-only server context for all threads */
static struct server_params {
    enum command cmd;
    const char *header;
    int headerSize;
    const char *url;
    int urlSize;
    const char *tailer;
    int tailerSize;
} server_context;

/* return size and update new received path to path pointer */
int receive_path(int rfd, char **path, char *buffer, int bufferSize)
{
    if (server_context.cmd == REDIRECT_HOST || server_context.cmd == PERMADIRECT_HOST) {
        char *recvEnd = buffer;
        char *ptr = buffer;
        char *pathBegin = NULL;
        char *pathEnd = NULL;

        while (ptr < buffer + bufferSize) {
            if (ptr == recvEnd) {
                /* recv more data */
                int bytes = recv(rfd, recvEnd, bufferSize - (recvEnd - buffer), 0);
                if (bytes == -1) {
                    perror("recv");
                    return -1;
                }
                if (bytes == 0) break;
                recvEnd += bytes;
            }

            char c = *ptr++;

            /* disallowed characters */
            if (c == '\r' || c == '\n') break;

            if (!pathBegin) {
                /* in METHOD */
                if (ptr - buffer > MAX_METHOD) break;
                if (c == ' ') {
                    pathBegin = ptr;
                }
            } else {
                /* in Request-URI */
                if (ptr - pathBegin > MAX_PATH) break;
                if (c == ' ') {
                    pathEnd = ptr-1;
                    break;
                }
            }
        }

        /* ignore rest of the data */
        recv(rfd, buffer, bufferSize, MSG_DONTWAIT|MSG_TRUNC);

        if (pathBegin && pathEnd && *pathBegin == '/') {
            /* path found */
            *path = pathBegin;
            return pathEnd - pathBegin;
        }

        /* no path - return empty path */
        *path = "";
        return 0;
    }

    /* ignore request for other commands */
    recv(rfd, buffer, bufferSize, MSG_TRUNC);
    *path = "";
    return 0;
}

/* serve the incoming client request */
void serve(int rfd)
{
    char buffer[BUFFER_SIZE];

    /* read requested path */
    char *path;
    int pathSize = receive_path(rfd, &path, buffer, sizeof(buffer));
    if (pathSize < 0) {
        close(rfd);
        return;
    }

    /* send response */
    send(rfd, server_context.header, server_context.headerSize, MSG_MORE);
    send(rfd, server_context.url,    server_context.urlSize,    MSG_MORE);
    send(rfd, path,                  pathSize,                  MSG_MORE);
    send(rfd, server_context.tailer, server_context.tailerSize, 0);

    /* tear down */
    shutdown(rfd, SHUT_RDWR);
    close(rfd);
    return;
}

/* serve the incoming client request using a new thread */
void serve_thread(int rfd)
{
    pid_t pid = fork_thread();
    if (pid < 0) {
        perror("clone3");
        close(rfd);
        return;
    }
    if (pid == 0) {
        // child
        serve(rfd);
        exit(0);
    }
    // parent
    return;
}

/* returns the first part of the response */
const char *get_header(enum command cmd)
{
    if (cmd == REDIRECT || cmd == REDIRECT_HOST) {
        return "HTTP/1.1 302 Found\r\n"
               "Location: ";
    } else if (cmd == PERMADIRECT || cmd == PERMADIRECT_HOST) {
        return "HTTP/1.1 301 Moved Permanently\r\n"
               "Location: ";
    }
    return NULL;
}

/* returns the last part of the response */
const char *get_tailer()
{
    return "\r\n"
           "Server: no80/" VERSION "\r\n"
           "Connection: close\r\n"
           "\r\n";
}

/* http server */
noreturn void server(int port, enum command cmd, const char *url)
{
    /* prepare shared read-only server context for all threads */
    server_context.cmd = cmd;
    server_context.header = get_header(cmd);
    server_context.headerSize = strlen(server_context.header);
    server_context.url = url;
    server_context.urlSize = strlen(url);
    server_context.tailer = get_tailer();
    server_context.tailerSize = strlen(server_context.tailer);

    unsigned long requests = 0;
    startTime = time(NULL);

    /* prepare listen socket */
    const int fd = listen_socket(port);

    /* serve incoming connections */
    while (1) {
        /* accept connection */
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
	            puts("again");
                continue;
            case EMFILE:
            case ENFILE:
                /* out of file descriptors */
                /* this limits how many connections can be processed simultaneously */
                /* adjust ulimit accordingly */
                puts("slowdown");
                usleep(100000); /* waiting 100ms for ongoing threads to finish */
                continue;
            }
            perror("accept");
            exit(2);
        }

#ifdef THREADING
        serve_thread(rfd);
#else
        serve(rfd);
#endif

        if ((++requests % 1000) == 0) {
            printf("+%lus: %luk redirects (%d/%d threads)\n", time(NULL) - startTime, requests / 1000, threadCount, maxThreadCount);
        }
    }
}

int main(int argc, char **argv)
{
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);
    signal(SIGCHLD, thread_exited);

    if (argc < 2) {
        puts("Usage: no80 [OPTION]... URL");
        puts("");
        puts("A redirecting http server");
        puts("");
        puts("Options:");
        puts("  -a    Append path from the http request to the redirected URL");
        puts("  -p    Redirect permanently using 301 instead of temporarily using 302");
        return 1;
    }

    int option_p = 0;
    int option_a = 0;

    for (int i = 1; i < argc-1; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            option_p = 1;
        } else if (strcmp(argv[i], "-a") == 0) {
            option_a = 1;
        } else {
            printf("Invalid parameter %s\n", argv[i]);
            return 1;
        }
    }

    enum command cmd = (option_p ? PERMADIRECT : REDIRECT) | (option_a ? REDIRECT_HOST : REDIRECT);
    const char *url = argv[argc-1];
    int port = 80;

    puts("no80 v" VERSION " - The resource effective redirecting http server");

    printf("Redirecting %sport %d requests to %s%s\n",
        ( option_p ? "permanently (301) " : "temporarily (302) "),
        port,
        url,
        ( option_a ? "</path>" : ""));

    server(port, cmd, url);
}
