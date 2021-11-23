/*
 * Starter code for proxy lab.
 * Feel free to modify this code in whatever way you wish.
 */

/* Some useful includes to help you get started */

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

void sigpipe_handler(int sig) {
    printf("SIGPIPE %d handled\n", sig);
    return;
}

/*
 * clienterror - returns an error message to the client
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg) {
    char buf[MAXLINE];
    char body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body,
            "%s<body bgcolor="
            "ffffff"
            ">\r\n",
            body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web Server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content_type: text/html\r\n");
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    rio_writen(fd, buf, strlen(buf));
    rio_writen(fd, body, strlen(body));
}

void build_http_request(rio_t *rio, const char *uri, const char *host,
                        const char *port, char *http_request) {
    char buf[MAXLINE], request_line[MAXLINE], header_host[MAXLINE],
        other_header[MAXLINE];

    sprintf(request_line, "GET %s HTTP/1.0\r\n", uri);
    while (rio_readlineb(rio, buf, MAXLINE) > 0) {
        if (!strcasecmp(buf, "\r\n")) {
            break;
        }

        if (!strncasecmp(buf, "User-Agent", strlen("User-Agent")) ||
            !strncasecmp(buf, "Connection", strlen("Connection")) ||
            !strncasecmp(buf, "Proxy_Connection", strlen("Proxy_Connection"))) {
            continue;
        }

        strcat(other_header, buf);
    }
    sprintf(header_host, "Host: %s:%s\r\n", host, port);
    sprintf(http_request, "%s%s%s%s%s%s\r\n", request_line, header_host,
            header_user_agent, header_connection, header_proxy_connection,
            other_header);
}

void doit(int fd) {
    char buf[MAXLINE];
    char http_request[MAXLINE];
    const char *method;
    const char *version;
    const char *uri;
    const char *host;
    const char *port;
    int serverfd;
    rio_t client_rio, server_rio;

    // read request line and headers
    rio_readinitb(&client_rio, fd);
    rio_readlineb(&client_rio, buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", buf);

    parser_t *parser = parser_new();
    parser_state state = parser_parse_line(parser, buf);
    if (state == ERROR) {
        clienterror(fd, buf, "400", "Bad Rrquest",
                    "Tiny could not handle the request");
        return;
    }

    parser_retrieve(parser, METHOD, &method);
    if (strcasecmp(method, "GET")) {
        clienterror(fd, buf, "501", "Not implemented",
                    "Tiny does not implement this method");
        return;
    }

    parser_retrieve(parser, HTTP_VERSION, &version);
    if (strcasecmp(version, "1.0") && strcasecmp(version, "1.1")) {
        clienterror(fd, buf, "400", "Bad Rrquest",
                    "Tiny could not handle the request");
        return;
    }

    parser_retrieve(parser, URI, &uri);
    parser_retrieve(parser, HOST, &host);
    parser_retrieve(parser, PORT, &port);

    build_http_request(&client_rio, uri, host, port, http_request);

    serverfd = open_clientfd(host, port);
    if (serverfd < 0) {
        fprintf(stderr, "Connection failed\n");
        return;
    }
    rio_readinitb(&server_rio, serverfd);
    rio_writen(serverfd, http_request, strlen(http_request));

    ssize_t n;
    while ((n = rio_readlineb(&server_rio, buf, MAXLINE)) > 0) {
        rio_writen(fd, buf, n);
    }

    parser_free(parser);
    close(serverfd);
    return;
}

int main(int argc, char **argv) {
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char hostname[MAXLINE], port[MAXLINE];

    // check command line arguments
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    Signal(SIGPIPE, sigpipe_handler);
    // signal(SIGPIPE, SIG_IGN);

    listenfd = open_listenfd(argv[1]);
    if (listenfd < 0) {
        fprintf(stderr, "Failed to listen on port: %s\n", argv[1]);
        exit(1);
    }

    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
        if (connfd < 0) {
            perror("accept");
            continue;
        }
        getnameinfo((struct sockaddr *)&clientaddr, clientlen, hostname,
                    MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        doit(connfd);
        close(connfd);
    }

    return 0;
}
