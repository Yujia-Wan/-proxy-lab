/**
 * @file proxy.c
 * @brief A tiny proxy program
 *
 * @author Yujia Wang <yujiawan@andrew.cmu.edu>
 */

/* Some useful includes to help you get started */

#include "cache.h"
#include "csapp.h"
#include "http_parser.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * Debug macros, which can be enabled by adding -DDEBUG in the Makefile
 * Use these if you find them useful, or delete them if not
 */
#ifdef DEBUG
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_assert(...)
#define dbg_printf(...)
#endif

/*
 * Max cache and object sizes
 * You might want to move these to the file containing your cache implementation
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)

/*
 * String to use for the User-Agent header.
 * Don't forget to terminate with \r\n
 */
static const char *header_user_agent = "User-Agent: Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20191101 Firefox/63.0.1\r\n";
static const char *header_connection = "Connection: close\r\n";
static const char *header_proxy_connection = "Proxy-Connection: close\r\n";

/*
 * clienterror - returns an error message to the client
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg) {
    char buf[MAXLINE];
    char body[MAXBUF];
    size_t buflen;
    size_t bodylen;

    /* Build the HTTP response body */
    bodylen = snprintf(body, MAXBUF,
                       "<html>\r\n"
                       "<head><title>Tiny Error</title></head>\r\n"
                       "<body bgcolor=\"ffffff\">\r\n"
                       "<h1>%s: %s</h1>\r\n"
                       "<p>%s: %s</p>\r\n"
                       "<hr><em>The Tiny Web server</em>\r\n"
                       "</body></html>\r\n",
                       errnum, shortmsg, longmsg, cause);
    if (bodylen >= MAXBUF) {
        return; // Overflow!
    }

    /* Build the HTTP response headers */
    buflen = snprintf(buf, MAXLINE,
                      "HTTP/1.0 %s %s\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      errnum, shortmsg, bodylen);
    if (buflen >= MAXLINE) {
        return; // Overflow!
    }

    /* Write the headers */
    if (rio_writen(fd, buf, buflen) < 0) {
        fprintf(stderr, "Error writing error response headers to client\n");
        return;
    }

    /* Write the body */
    if (rio_writen(fd, body, bodylen) < 0) {
        fprintf(stderr, "Error writing error response body to client\n");
        return;
    }
}

void build_http_request(rio_t *rio, char *request_line, char *header_host,
                        char *http_request) {
    char buf[MAXLINE];
    char other_header[MAXLINE];

    while (rio_readlineb(rio, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n")) {
            break;
        }

        if (!strncasecmp(buf, "Host", strlen("Host"))) {
            sprintf(header_host, buf);
            continue;
        }

        if (strncasecmp(buf, "User-Agent", strlen("User-Agent")) &&
            strncasecmp(buf, "Connection", strlen("Connection")) &&
            strncasecmp(buf, "Proxy-Connection", strlen("Proxy-Connection"))) {
            strcat(other_header, buf);
        }
    }

    sprintf(http_request, "%s%s%s%s%s%s\r\n", request_line, header_host,
            header_user_agent, header_connection, header_proxy_connection,
            other_header);
}

void doit(int fd) {
    char buf[MAXLINE];
    char request_line[MAXLINE];
    char header_host[MAXLINE];
    char http_request[MAXLINE];
    const char *method;
    const char *version;
    const char *uri;
    const char *host;
    const char *port;
    const char *path;
    int serverfd;
    rio_t client_rio, server_rio;
    ssize_t n;

    // read request line
    rio_readinitb(&client_rio, fd);
    rio_readlineb(&client_rio, buf, MAXLINE);

    // header_t *header;
    parser_t *parser = parser_new();
    parser_state state = parser_parse_line(parser, buf);

    if (state == ERROR) {
        clienterror(fd, buf, "400", "Bad Request",
                    "Tiny could not handle this request (ERROR)");
        return;
    }

    parser_retrieve(parser, METHOD, &method);
    if (strcasecmp(method, "GET")) {
        clienterror(fd, buf, "501", "Not implemented",
                    "Tiny does not implement this method");
        return;
    }

    parser_retrieve(parser, HTTP_VERSION, &version);
    if (strncasecmp(version, "1.0", strlen("1.0")) &&
        strncasecmp(version, "1.1", strlen("1.1"))) {
        clienterror(fd, buf, "400", "Bad Request",
                    "Tiny could not handle this request (HTTP_VERSION)");
        return;
    }

    // read cache and if in the cache, write back to the client
    parser_retrieve(parser, URI, &uri);
    if ((n = read_cache(uri, fd)) > 0) {
        return;
    }

    parser_retrieve(parser, HOST, &host);
    parser_retrieve(parser, PORT, &port);
    parser_retrieve(parser, PATH, &path);

    sprintf(request_line, "GET %s HTTP/1.0\r\n", path);
    sprintf(header_host, "Host: %s:%s\r\n", host, port);

    build_http_request(&client_rio, request_line, header_host, http_request);

    // if not in the cache, establish connection to the web server
    serverfd = open_clientfd(host, port);
    if (serverfd < 0) {
        fprintf(stderr, "Connection failed\n");
        return;
    }

    // request the object the client specified
    rio_readinitb(&server_rio, serverfd);
    rio_writen(serverfd, http_request, strlen(http_request));

    // read the server's response and forward it to the client
    ssize_t response_size = 0;
    char response[MAX_OBJECT_SIZE];
    char *responsep = response;
    while ((n = rio_readnb(&server_rio, buf, MAXLINE)) > 0) {
        rio_writen(fd, buf, n);
        // store the web server's response if maximum object size is not
        // exceeded
        response_size += n;
        if (response_size < MAX_OBJECT_SIZE) {
            memcpy(responsep, buf, n);
            responsep += n;
        }
    }

    // write the web object into cache
    if (response_size < MAX_OBJECT_SIZE) {
        write_cache(uri, response, response_size);
    }

    parser_free(parser);
    close(serverfd);
    return;
}

void *thread(void *vargp) {
    int connfd = *((int *)vargp);
    pthread_detach(pthread_self());
    free(vargp);
    doit(connfd);
    close(connfd);
    return NULL;
}

int main(int argc, char **argv) {
    int listenfd;
    int *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char host[MAXLINE];
    char port[MAXLINE];
    pthread_t tid;

    // check command line arguments
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);

    init_cache();

    listenfd = open_listenfd(argv[1]);
    if (listenfd < 0) {
        fprintf(stderr, "Failed to listen on port: %s\n", argv[1]);
        exit(1);
    }

    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = malloc(sizeof(int));
        *connfdp = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
        if (*connfdp < 0) {
            perror("accept error");
            continue;
        }
        getnameinfo((struct sockaddr *)&clientaddr, clientlen, host, MAXLINE,
                    port, MAXLINE, 0);
        sio_printf("Accepted connection from (%s, %s)\n", host, port);
        pthread_create(&tid, NULL, thread, connfdp);
    }

    free_cache();

    return 0;
}
