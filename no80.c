#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <linux/sched.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdnoreturn.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define STR(s) #s
#define STR_EVAL(e) STR(e)
#define VERSION_STR STR_EVAL(VERSION)

/* TCP connection queue length */
#define QUEUE_LENGTH 1000

/* Read buffer size */
#define BUFFER_SIZE 8192

/* Cooldown time microseconds */
#define COOLDOWN_TIME 100000

/* Enable threading */
#define THREADING
#define MAX_THREADS_LIMIT 2000

/* Max HTTP protocol element sizes */
#define MAX_METHOD 10
#define MAX_PATH 8000

enum command { REDIRECT = 0, REDIRECT_HOST = 1, PERMADIRECT = 2, PERMADIRECT_HOST = 3 };

/* globals */
_Atomic int threads = 0;
int maxThreads = 0;
time_t startTime;
_Atomic unsigned long requests = 0;
_Atomic unsigned long successes = 0;

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
    return sys_clone3(&ca, sizeof(ca));
}

/* signal handler SIGINTR and SIGTERM */
noreturn void interrupted(int sig)
{
    _exit(0); /* handler safe exit closes open sockets */
}

/* signal handler SIGCHLD */
void thread_exited(int sig)
{
    /* kill the zombies */
    int status = 0;
    while (waitpid(-1, &status, WUNTRACED | WNOHANG) > 0) {
        --threads;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) ++successes;
    }
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
int serve(int rfd)
{
    char buffer[BUFFER_SIZE];

    /* read requested path */
    char *path;
    int pathSize = receive_path(rfd, &path, buffer, sizeof(buffer));
    if (pathSize < 0) {
        close(rfd);
        return -1;
    }

    /* send response */
    send(rfd, server_context.header, server_context.headerSize, MSG_MORE);
    send(rfd, server_context.url, server_context.urlSize, MSG_MORE);
    if (pathSize > 0) {
        send(rfd, path, pathSize, MSG_MORE);
    }
    send(rfd, server_context.tailer, server_context.tailerSize, 0);

    /* tear down */
    shutdown(rfd, SHUT_RDWR);
    close(rfd);
    return 0;
}

/* serve the incoming client request using a new thread */
void serve_thread(int rfd)
{
    if (threads >= MAX_THREADS_LIMIT) {
        close(rfd);
        /* thread limit reached */
        sched_yield(); /* let ongoing threads to finish */
        return;
    }
    pid_t pid = fork_thread();
    if (pid < 0) {
        // error
        perror("clone3");
        close(rfd);
        return;
    }
    if (pid == 0) {
        // child
        exit(serve(rfd));
    }
    // parent
    int tc = ++threads;
    if (tc > maxThreads) maxThreads = tc;
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
           "Server: no80/" VERSION_STR "\r\n"
           "Connection: close\r\n"
           "\r\n";
}

void printStats()
{
    printf("+%lus: %luk requests (%lu failures) (%d/%d threads)\n",
        time(NULL) - startTime,
        requests / 1000,
        requests - successes - threads,
        threads,
        maxThreads);
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

    startTime = time(NULL);

    /* prepare listen socket */
    const int fd = listen_socket(port);

    /* serve incoming connections */
    while (1) {
        /* accept connection */
        const int rfd = accept(fd, NULL, NULL);
        ++requests;
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
                /* adjust ulimit accordingly */
                sched_yield(); /* let ongoing threads to finish */
                continue;
            }
            perror("accept");
            exit(2);
        }

#ifdef THREADING
        serve_thread(rfd);
#else
        if (serve(rfd) == 0) ++successes;
#endif

        if ((requests % 1000) == 0) printStats();
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

    puts("no80 v" VERSION_STR " - The resource effective redirecting http server");

    printf("Redirecting %s port %d requests to %s%s\n",
        ( option_p ? "permanently (301)" : "temporarily (302)"),
        port,
        url,
        ( option_a ? "</path>" : ""));

    server(port, cmd, url);
}
