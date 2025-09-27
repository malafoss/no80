/*
 * no80 - https://github.com/malafoss/no80
 *
 * Copyright (c) 2025 Mikko Ala-Fossi
 *
 * Licensed under MIT license
 */
#define STR(s) #s
#define STR_EVAL(e) STR(e)
#define VERSION_STR STR_EVAL(VERSION)
const char *plate_text = "The resource effective redirecting http server v" VERSION_STR;

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

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/error-ssl.h>

/* Environment assumptions */
static_assert(EAGAIN == EWOULDBLOCK);

/* TCP parameters */
#define QUEUE_LENGTH 1000

/* Read buffer size */
#define BUFFER_SIZE 8192

/* Max HTTP protocol element sizes */
#define MAX_METHOD 10
#define MAX_HTTP_PATH 8000
static_assert(BUFFER_SIZE > MAX_METHOD + MAX_HTTP_PATH + 128);

/* Epoll parameters */
#define MAX_EPOLL_CREATES 256
#define MAX_EPOLL_EVENTS 256
#define EPOLL_TIMEOUT_MS 60*1000

/* redirecting command mode */
enum command { REDIRECT = 0, PERMADIRECT = 1 };

/* maximum number of matched path redirections */
#define MAX_MATCHES 256

/* statistics structure */
struct statistics {
    bool noStatistics;
    time_t startTime;
    unsigned long requests;
    unsigned long successes;
    unsigned long completed;
    int connections; /* ignore efd */
    int maxConns;
    int maxEvents;
};

/* global statistics instance */
static struct statistics stats = {
    .noStatistics = false,
    .startTime = 0,
    .requests = 0,
    .successes = 0,
    .completed = 0,
    .connections = -1,
    .maxConns = 0,
    .maxEvents = 0
};

/* signal handler SIGINTR and SIGTERM */
noreturn void interrupted(int sig)
{
    _exit(0); /* handler safe exit closes open sockets */
}

/* fatal error handler */
static noreturn void fatal_error(const char *msg)
{
    perror(msg);
    exit(2);
}


/* returns listen socket fd */
int listen_socket(int port)
{
    /* prepare socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        fatal_error("socket");
    }

    /* enable address reusage */
    {
        int option = 1; /* enable */
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option))) {
            fatal_error("setsockopt");
        }
    }

    /* enable immediate send */
    {
        int option = 1; /* enable */
        if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &option, sizeof(option))) {
            fatal_error("setsockopt");
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
            fatal_error("bind");
        }
    }

    /* list the port */
    if (listen(fd, QUEUE_LENGTH) == -1) {
        fatal_error("listen");
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

    /* SSL parameters */
    WOLFSSL *ssl;
    bool is_ssl;
};

/* epoll helper functions */
static int epoll_add_connection(int efd, struct connect_params *cp)
{
    if (epoll_ctl(efd, EPOLL_CTL_ADD, cp->fd, &(cp->ee)) < 0) {
        perror("epoll_ctl add");
        return -1;
    }
    return 0;
}

static void epoll_remove_connection(int efd, int fd)
{
    if (epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        perror("epoll_ctl del");
    }
}

/* SSL error handling helper */
static int handle_ssl_error(WOLFSSL *ssl, int ret)
{
    int err = wolfSSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return -2; // EAGAIN equivalent
    }
    return -1; // Other error
}

/* forward declaration */
void free_connect(struct connect_params *cp);

/* connection cleanup helper */
static void cleanup_connection(int efd, struct connect_params *cp)
{
    epoll_remove_connection(efd, cp->fd);
    free_connect(cp);
}

/* SSL context */
static WOLFSSL_CTX *ssl_ctx = NULL;

/* Initialize WolfSSL */
int init_wolfssl(const char *cert_file, const char *key_file)
{
    /* Initialize wolfSSL with debugging */
    /* wolfSSL_Debugging_ON(); */
    wolfSSL_Init();
    
    /* Create and initialize WOLFSSL_CTX - Try TLS 1.3 first, fallback to TLS 1.2 */
    ssl_ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (ssl_ctx == NULL) {
        /* TLS 1.3 not available, fallback to TLS 1.2 */
        ssl_ctx = wolfSSL_CTX_new(wolfTLSv1_2_server_method());
        if (ssl_ctx == NULL) {
            fprintf(stderr, "ERROR: failed to create WOLFSSL_CTX\n");
            return -1;
        }
        printf("Using TLS 1.2 (TLS 1.3 not available)\n");
    } else {
        printf("Using TLS 1.3\n");
    }
    
    /* Set verification mode to none - accept all clients */
    wolfSSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, 0);
    
    /* Ignore certificate validation errors */
    wolfSSL_CTX_set_verify_depth(ssl_ctx, 0);
    
    /* Set up to ignore certificate alerts from clients */
    wolfSSL_CTX_set_servername_callback(ssl_ctx, NULL);
    
    /* Disable session caching as requested */
    wolfSSL_CTX_set_session_cache_mode(ssl_ctx, WOLFSSL_SESS_CACHE_OFF);
    
    /* Optimize cipher suites for performance */
    const char* fast_ciphers = 
        "TLS13-AES128-GCM-SHA256:"           /* TLS 1.3 fastest */
        "TLS13-CHACHA20-POLY1305-SHA256:"    /* TLS 1.3 ChaCha20 */
        "ECDHE-RSA-AES128-GCM-SHA256:"       /* TLS 1.2 fast ECDHE */
        "ECDHE-RSA-CHACHA20-POLY1305";       /* TLS 1.2 ChaCha20 */

    if (wolfSSL_CTX_set_cipher_list(ssl_ctx, fast_ciphers) != SSL_SUCCESS) {
        fprintf(stderr, "WARNING: Could not set optimized cipher list, using defaults\n");
    }
    
    /* Additional performance optimizations */
    wolfSSL_CTX_set_options(ssl_ctx, WOLFSSL_OP_NO_SSLv2 | WOLFSSL_OP_NO_SSLv3);
    wolfSSL_CTX_SetMinVersion(ssl_ctx, WOLFSSL_TLSV1_2); /* Minimum TLS 1.2 */
    
    /* Load server certificates */
    if (wolfSSL_CTX_use_certificate_file(ssl_ctx, cert_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "ERROR: failed to load certificate file %s\n", cert_file);
        return -1;
    }
    
    /* Load server key */
    if (wolfSSL_CTX_use_PrivateKey_file(ssl_ctx, key_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "ERROR: failed to load key file %s\n", key_file);
        return -1;
    }
    
    return 0;
}

/* Cleanup WolfSSL */
void cleanup_wolfssl()
{
    if (ssl_ctx) {
        wolfSSL_CTX_free(ssl_ctx);
        ssl_ctx = NULL;
    }
    wolfSSL_Cleanup();
}

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

/* SSL read - returns bytes read, -1 on error, -2 on EAGAIN */
int ssl_read(struct connect_params *cp, char *buf, int size)
{
    if (!cp->ssl) return -1;
    
    int ret = wolfSSL_read(cp->ssl, buf, size);
    if (ret <= 0) {
        int ssl_result = handle_ssl_error(cp->ssl, ret);
        if (ssl_result == -2) {
            return -2; // EAGAIN equivalent
        }
        int err = wolfSSL_get_error(cp->ssl, ret);
        if (err == -313) {
            return -1; // Ignore -313 related to key validation
        }
        fprintf(stderr, "wolfSSL_read error: %d\n", err);
        return -1;
    }
    return ret;
}

/* SSL write - returns bytes written, -1 on error, -2 on EAGAIN */
int ssl_write(struct connect_params *cp, const char *buf, int size)
{
    if (!cp->ssl) return -1;
    
    int ret = wolfSSL_write(cp->ssl, buf, size);
    if (ret <= 0) {
        int ssl_result = handle_ssl_error(cp->ssl, ret);
        if (ssl_result == -2) {
            return -2; // EAGAIN equivalent
        }
        int err = wolfSSL_get_error(cp->ssl, ret);
        fprintf(stderr, "wolfSSL_write error: %d\n", err);
        return -1;
    }
    return ret;
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
            int bytes;
            
            if (cp->is_ssl) {
                bytes = ssl_read(cp, cp->recvPtr, bufferSize - (cp->recvPtr - cp->buffer));
                if (bytes == -2) return 1; // EAGAIN equivalent
            } else {
                bytes = recv(cp->fd, cp->recvPtr, bufferSize - (cp->recvPtr - cp->buffer), MSG_DONTWAIT);
                if (bytes == -1) {
                    if (errno == EAGAIN) return 1; // recv more later
                    perror("recv");
                    return -1;
                }
            }
            if (bytes <= 0) return -1; // needed bytes, so its error
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
            if (ptr - cp->pathBegin > MAX_HTTP_PATH || c == ' ') {
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
    if (!stats.noStatistics) {
        printf("+%lus: %lu requests (%lu successes, %lu failures, %d ongoing) (%d max events, %d max conns)\n",
            time(NULL) - stats.startTime,
            stats.requests,
            stats.successes,
            stats.completed - stats.successes,
            stats.connections,
            stats.maxEvents,
            stats.maxConns);
    }
    __transaction_atomic {
        stats.maxEvents = 0;
        stats.maxConns = 0;
    }
}

/* allocate new connection parameters */
struct connect_params *new_connect(int fd)
{
    struct connect_params *c = malloc(sizeof(struct connect_params));
    if (!c) return NULL;
    bzero(c, sizeof(struct connect_params));
    c->fd = fd;
    __transaction_atomic {
        ++stats.connections;
        if (stats.connections > stats.maxConns) stats.maxConns = stats.connections;
    }
    return c;
}

/* Setup SSL for a connection */
int setup_ssl_connection(struct connect_params *cp)
{
    if (!ssl_ctx) return -1;
    
    /* Create a new SSL object */
    if ((cp->ssl = wolfSSL_new(ssl_ctx)) == NULL) {
        fprintf(stderr, "ERROR: failed to create WOLFSSL object\n");
        return -1;
    }
    
    /* Associate the socket with the SSL object */
    if (wolfSSL_set_fd(cp->ssl, cp->fd) != SSL_SUCCESS) {
        fprintf(stderr, "ERROR: failed to set socket to SSL object\n");
        wolfSSL_free(cp->ssl);
        cp->ssl = NULL;
        return -1;
    }
    
    /* Mark this connection as SSL */
    cp->is_ssl = true;
    
    return 0;
}

int send_part(struct connect_params *cp, const char *msg, int msgsize, int *sent, bool last)
{
    int rc;
    
    if (cp->is_ssl) {
        rc = ssl_write(cp, msg + *sent, msgsize - *sent);
        if (rc == -2) return 1; // EAGAIN equivalent
        if (rc < 0) return -1;
    } else {
        rc = send(cp->fd, msg + *sent, msgsize - *sent, MSG_DONTWAIT | (last ? 0 : MSG_MORE));
        if (rc < 0) {
            if (errno == EAGAIN) return 1;
            perror("send");
            return -1;
        }
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
        rc = send_part(cp, serverContext.header, serverContext.headerSize, &(cp->headerSent), 0);
        if (rc != 0) return rc;
    }

    if (cp->urlSize > cp->urlSent) {
        rc = send_part(cp, cp->url, cp->urlSize, &(cp->urlSent), 0);
        if (rc != 0) return rc;
    }

    int pathSize = cp->pathEnd - cp->pathBegin;
    if (pathSize > cp->pathSent) {
        rc = send_part(cp, cp->pathBegin, pathSize, &(cp->pathSent), 0);
        if (rc != 0) return rc;
    }

    if (serverContext.tailerSize > cp->tailerSent) {
        rc = send_part(cp, serverContext.tailer, serverContext.tailerSize, &(cp->tailerSent), 1);
        if (rc != 0) return rc;
    }

    /* tear down */
    if (cp->is_ssl) {
        /* Properly shutdown the SSL connection */
        wolfSSL_shutdown(cp->ssl);
    }
    shutdown(cp->fd, SHUT_RDWR);
    return 0;
}

/* free allocated connection parameters */
void free_connect(struct connect_params *cp)
{
    if (cp) {
        if (cp->ssl) {
            wolfSSL_shutdown(cp->ssl);
            wolfSSL_free(cp->ssl);
        }
        close(cp->fd);
        free(cp);
    }
    __transaction_atomic {
        --stats.connections;
        ++stats.completed;
    }
}

/* http/https server */
noreturn void server(int port, enum command cmd, const char *url, bool append, struct match_params *pathMatch, int matches, bool is_ssl)
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

    stats.startTime = time(NULL);

    /* prepare listen socket */
    const int fd = listen_socket(port);

    const int efd = epoll_create(MAX_EPOLL_CREATES);
    if (efd == -1) {
        fatal_error("epoll_create");
    }

    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = new_connect(fd);
    if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ee) < 0) {
        fatal_error("epoll_ctl");
    }

    struct epoll_event ees[MAX_EPOLL_EVENTS];

    /* serve incoming events */
    while (1) {
        int count = epoll_wait(efd, ees, sizeof(ees)/sizeof(ees[0]), EPOLL_TIMEOUT_MS);
        if (count == -1) {
            fatal_error("epoll_wait");
        }
        if (count == 0) {
            /* timeout */
            if (stats.maxEvents > 0) {
                print_stats();
            }
            continue;
        }
        __transaction_atomic {
            if (count > stats.maxEvents) stats.maxEvents = count;
        }

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
                        fatal_error("accept");
                    }
                }

                /* nonblocking socket */
                fcntl(rfd, F_SETFL, fcntl(rfd, F_GETFL) | O_NONBLOCK);

                /* new connection -> EPOLLIN event */
                struct connect_params *cp = new_connect(rfd);
                
                /* Setup SSL if this is an SSL server */
                if (is_ssl) {
                    if (setup_ssl_connection(cp) < 0) {
                        free_connect(cp);
                        continue;
                    }
                    
                    /* We'll handle the SSL handshake in the EPOLLIN event handler */
                    /* Just add the connection to epoll for now */
                    cp->ee.events = EPOLLIN;
                    cp->ee.data.ptr = cp;
                    if (epoll_add_connection(efd, cp) < 0) {
                        free_connect(cp);
                        continue;
                    }
                    continue;
                }
                
                cp->ee.events = EPOLLIN;
                cp->ee.data.ptr = cp;
                if (epoll_add_connection(efd, cp) < 0) {
                    free_connect(cp);
                    continue;
                }

                bool doprint;
                __transaction_atomic {
                    ++stats.requests;
                    doprint = ((stats.requests % 1000) == 0);
                }
                if (doprint) print_stats();
                continue;
            }

            /* handle connection events */
            if (ees[i].events & EPOLLIN) {
                /* If this is an SSL connection, perform the handshake if needed */
                if (cp->is_ssl) {
                    /* Try to accept the SSL connection */
                    int ret = wolfSSL_accept(cp->ssl);
                    if (ret != SSL_SUCCESS) {
                        int ssl_result = handle_ssl_error(cp->ssl, ret);
                        if (ssl_result == -2) {
                            /* Handshake needs more data, will continue later */
                            continue;
                        }
                        
                        /* For certain errors, we can continue anyway */
                        int err = wolfSSL_get_error(cp->ssl, ret);
                        if (err == -313) { /* ASN_NO_SIGNER_E - Client doesn't trust our certificate */
                            /* Continue despite the error */
                        } else {
                            /* For other errors, close the connection */
                            cleanup_connection(efd, cp);
                            continue;
                        }
                    }
                }
                /* read request */
                int rc = read_request(cp);
                if (rc < 0) {
                    /* failure -> delete event */
                    cleanup_connection(efd, cp);
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
                __transaction_atomic {
                    if (rc == 0) ++stats.successes;
                }
                cleanup_connection(efd, cp);
                continue;
            }
            /* unknown event */
        }
    }
}

/* print help text */
void print_help()
{
    printf("Usage: no80 [OPTION]... URL\n"
           "\n"
           "%s\n"
           "\n"
           "Options:\n"
           "  -a           Append path from the http request to the redirected URL\n"
           "  -h           Print this help text and exit\n"
           "  -m PATH URL  Redirect path matching with PATH to URL\n"
           "  -s PATH URL  Redirect path starting with PATH to URL\n"
           "  -r PATH URL  Redirect path starting with PATH to URL appended with the rest of the path\n"
           "  -p N         Use specified port number N (default is port 80)\n"
           "  -P           Redirect permanently using 301 instead of temporarily using 302\n"
           "  -q           Suppress statistics\n"
           "  -S N         Use specified HTTPS port number N (default is port 443)\n"
           "  -c FILE      SSL certificate file in PEM format (default: /certs/cert.pem)\n"
           "  -k FILE      SSL private key file in PEM format (default: /certs/key.pem)\n"
           ,
        plate_text);
}

int main(int argc, char **argv)
{
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);

    if (argc < 2) {
        print_help();
        return 1;
    }

    bool option_P = 0;
    bool option_a = 0;
    int port = 80;
    const char *url = NULL;
    int matches = 0;
    int https_port = 443;  /* Default HTTPS port */
    const char *cert_file = "/certs/cert.pem";  /* Default certificate file */
    const char *key_file = "/certs/key.pem";    /* Default key file */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-P") == 0) {
            option_P = 1;
        } else if (strcmp(argv[i], "-a") == 0) {
            option_a = 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            stats.noStatistics = 1;
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
        } else if (strcmp(argv[i], "-S") == 0) {
            if (++i < argc) {
                https_port = atoi(argv[i]);
                if (https_port < 1 || https_port > 65535) {
                    puts("Invalid HTTPS port number specified");
                    return 1;
                }
            } else {
                puts("Missing HTTPS port number");
                return 1;
            }
        } else if (strcmp(argv[i], "-c") == 0) {
            if (++i < argc) {
                cert_file = argv[i];
            } else {
                puts("Missing certificate file");
                return 1;
            }
        } else if (strcmp(argv[i], "-k") == 0) {
            if (++i < argc) {
                key_file = argv[i];
            } else {
                puts("Missing key file");
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
                        if (m[0] != '/') {
                            puts("Path should start with /");
                            return 1;
                        }
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
            print_help();
            return 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("no80 - %s\n", plate_text);
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

    printf("no80 - %s\n"
           "Redirecting requests %s\n",
        plate_text,
        ( option_P ? "permanently (301)" : "temporarily (302)"));

    for (int i = 0; i < matches; i++)
        printf("  Path %s%s to URL %s%s\n",
            pathMatch[i].path,
            (pathMatch[i].begin ? "*" : ""),
            pathMatch[i].url,
            (pathMatch[i].append ? "*" : ""));

    printf("  Path * to URL %s%s\n",
        url,
        ( option_a ? "*" : ""));

    /* Start HTTPS server */
    {
        /* Check if certificate and key files are provided */
        if (!cert_file) {
            puts("Missing certificate file (-c option)");
            return 1;
        }
        if (!key_file) {
            puts("Missing key file (-k option)");
            return 1;
        }
        
        /* Initialize WolfSSL */
        if (init_wolfssl(cert_file, key_file) < 0) {
            puts("Failed to initialize WolfSSL");
            return 1;
        }
        
        /* Start HTTPS server in a child process */
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        } else if (pid == 0) {
            /* Child process - HTTPS server */
            printf("Starting HTTPS server on port %d\n", https_port);
            server(https_port, cmd, url, append, pathMatch, matches, true);
            /* Never returns */
        }
        
        /* Parent process continues with HTTP server */
    }

    /* Start HTTP server */
    printf("Starting HTTP server on port %d\n", port);
    server(port, cmd, url, append, pathMatch, matches, false);
    /* Never returns */
}
