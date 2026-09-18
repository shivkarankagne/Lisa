#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../retrieval/retrieval.h"
#include "../storage/storage.h"

#include "../kernels/arm64/lisa_asm.h"

#define LISA_HTTP_MAX_BODY (4 * 1024 * 1024)   /* 4 MB cap */
#define LISA_HTTP_BUF      8192

static volatile sig_atomic_t g_running = 1;
static volatile int g_listen_fd = -1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

/* ---- helpers ---- */

static int write_all(int fd, const char* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int send_status(int fd, int code, const char* reason,
                       const char* body, size_t body_len) {
    char header[512];
    int hn = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, reason, body_len);
    if (hn < 0 || (size_t)hn >= sizeof(header)) return -1;
    if (write_all(fd, header, (size_t)hn) != 0) return -1;
    if (body_len > 0) {
        if (write_all(fd, body, body_len) != 0) return -1;
    }
    return 0;
}

static int send_error(int fd, int code, const char* reason) {
    char body[128];
    int bn = snprintf(body, sizeof(body), "%d %s\n", code, reason);
    if (bn < 0) bn = 0;
    return send_status(fd, code, reason, body, (size_t)bn);
}

/* ---- request parsing ---- */

/*
 * We do a single recv to read the request line and headers, then
 * read the body according to Content-Length.
 *
 * This is deliberately minimal:
 *   - one request per connection
 *   - Content-Length required for bodies
 *   - no chunked encoding
 */

typedef struct {
    char method[16];
    char path[1024];
    char query[1024];
    size_t content_length;
    int has_content_length;
} http_request_t;

static int parse_request_line(const char* line, http_request_t* req) {
    /* METHOD SP PATH SP VERSION */
    const char* p = line;
    const char* sp1 = strchr(p, ' ');
    if (!sp1) return -1;
    size_t mlen = (size_t)(sp1 - p);
    if (mlen == 0 || mlen >= sizeof(req->method)) return -1;
    memcpy(req->method, p, mlen);
    req->method[mlen] = '\0';

    p = sp1 + 1;
    const char* sp2 = strchr(p, ' ');
    if (!sp2) return -1;
    size_t plen = (size_t)(sp2 - p);
    if (plen == 0 || plen >= sizeof(req->path)) return -1;

    /* split path and query on '?' */
    const char* q = memchr(p, '?', plen);
    if (q) {
        size_t pathlen = (size_t)(q - p);
        size_t qlen = plen - pathlen - 1;
        if (pathlen >= sizeof(req->path)) return -1;
        if (qlen >= sizeof(req->query)) return -1;
        memcpy(req->path, p, pathlen);
        req->path[pathlen] = '\0';
        memcpy(req->query, q + 1, qlen);
        req->query[qlen] = '\0';
    } else {
        memcpy(req->path, p, plen);
        req->path[plen] = '\0';
        req->query[0] = '\0';
    }

    return 0;
}

static int parse_header_line(const char* line, http_request_t* req) {
    /* We only care about Content-Length. Case-insensitive name match. */
    const char* colon = strchr(line, ':');
    if (!colon) return 0;
    size_t nlen = (size_t)(colon - line);
    if (nlen == 14 &&
        strncasecmp(line, "Content-Length", 14) == 0) {
        long v = strtol(colon + 1, NULL, 10);
        if (v < 0) return -1;
        req->content_length = (size_t)v;
        req->has_content_length = 1;
    }
    return 0;
}

/* read the full request into a buffer, then parse */
static int read_request(int fd, http_request_t* req,
                        char** body_out, size_t* body_len_out) {
    char buf[LISA_HTTP_BUF];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return -1;
    buf[n] = '\0';

    /* find end of headers */
    char* hdr_end = strstr(buf, "\r\n\r\n");
    size_t hdr_len;
    size_t body_in_buf = 0;
    char* body_start = NULL;

    if (hdr_end) {
        hdr_len = (size_t)(hdr_end - buf) + 4;
        body_start = buf + hdr_len;
        body_in_buf = (size_t)n - hdr_len;
    } else {
        /* headers not fully in first read; we do not support that */
        return -1;
    }

    /* parse request line */
    char* line_end = strstr(buf, "\r\n");
    if (!line_end) return -1;
    *line_end = '\0';
    if (parse_request_line(buf, req) != 0) return -1;

    /* parse headers */
    char* p = line_end + 2;
    while (p < hdr_end) {
        char* e = strstr(p, "\r\n");
        if (!e || e > hdr_end) break;
        *e = '\0';
        if (*p == '\0') break;
        if (parse_header_line(p, req) != 0) return -1;
        p = e + 2;
    }

    /* determine body length */
    size_t body_len = 0;
    if (req->has_content_length) {
        body_len = req->content_length;
        if (body_len > LISA_HTTP_MAX_BODY) return -2;
    }

    if (body_len > 0) {
        char* body = (char*)malloc(body_len);
        if (!body) return -3;
        size_t have = body_in_buf < body_len ? body_in_buf : body_len;
        memcpy(body, body_start, have);
        size_t got = have;
        while (got < body_len) {
            ssize_t r = recv(fd, body + got, body_len - got, 0);
            if (r <= 0) { free(body); return -1; }
            got += (size_t)r;
        }
        *body_out = body;
        *body_len_out = body_len;
    } else {
        *body_out = NULL;
        *body_len_out = 0;
    }

    return 0;
}

/* ---- query string helpers ---- */

static int query_get(const char* query, const char* key,
                     char* out, size_t out_len) {
    size_t klen = strlen(key);
    const char* p = query;
    while (*p) {
        const char* amp = strchr(p, '&');
        const char* end = amp ? amp : p + strlen(p);
        const char* eq = memchr(p, '=', (size_t)(end - p));
        if (eq) {
            size_t nlen = (size_t)(eq - p);
            if (nlen == klen && strncmp(p, key, klen) == 0) {
                size_t vlen = (size_t)(end - eq - 1);
                if (vlen >= out_len) return -1;
                memcpy(out, eq + 1, vlen);
                out[vlen] = '\0';
                return 0;
            }
        }
        p = amp ? amp + 1 : end;
    }
    return -1;
}

/* ---- routes ---- */

static int handle_health(int fd) {
    return send_status(fd, 200, "OK", "ok\n", 3);
}

static int handle_search(int fd, const http_request_t* req,
                         const char* body, size_t body_len) {
    char coll[1024];
    char topk_s[32];

    if (query_get(req->query, "collection", coll, sizeof(coll)) != 0) {
        return send_error(fd, 400, "Bad Request");
    }
    if (query_get(req->query, "topk", topk_s, sizeof(topk_s)) != 0) {
        strcpy(topk_s, "5");
    }

    int topk = atoi(topk_s);
    if (topk <= 0) {
        return send_error(fd, 400, "Bad Request");
    }

    int handle = storage_open(coll);
    if (handle < 0) {
        return send_error(fd, 404, "Not Found");
    }

    int n = storage_get_n(handle);
    int dim = storage_get_dim(handle);
    const float* vectors = storage_get_vectors(handle);

    if (n <= 0 || dim <= 0 || !vectors) {
        storage_close(handle);
        return send_error(fd, 500, "Internal Server Error");
    }

    size_t expected = (size_t)dim * sizeof(float);
    if (body_len != expected) {
        storage_close(handle);
        return send_error(fd, 400, "Bad Request");
    }

    const float* query = (const float*)body;

    int k_eff = (topk < n) ? topk : n;

    int* indices = (int*)malloc((size_t)k_eff * sizeof(int));
    float* dists = (float*)malloc((size_t)k_eff * sizeof(float));
    if (!indices || !dists) {
        free(indices); free(dists);
        storage_close(handle);
        return send_error(fd, 500, "Internal Server Error");
    }

    lisa_result_t r = {
        .indices = indices,
        .dists = dists,
        .k = k_eff,
        .n_returned = 0
    };

    int rc = lisa_search_exact_asm(query, vectors, n, dim, k_eff, &r);
    if (rc != 0) {
        free(indices); free(dists);
        storage_close(handle);
        return send_error(fd, 500, "Internal Server Error");
    }

    /* build response body */
    size_t cap = (size_t)k_eff * 64 + 1;
    char* out = (char*)malloc(cap);
    if (!out) {
        free(indices); free(dists);
        storage_close(handle);
        return send_error(fd, 500, "Internal Server Error");
    }
    size_t off = 0;
    for (int i = 0; i < r.n_returned; i++) {
        int wn = snprintf(out + off, cap - off, "%d %.6f\n",
                          indices[i], dists[i]);
        if (wn < 0 || (size_t)wn >= cap - off) break;
        off += (size_t)wn;
    }

    int s = send_status(fd, 200, "OK", out, off);

    free(out);
    free(indices);
    free(dists);
    storage_close(handle);
    return s;
}

/* ---- connection loop ---- */

static void handle_connection(int fd) {
    http_request_t req;
    memset(&req, 0, sizeof(req));

    char* body = NULL;
    size_t body_len = 0;

    int pr = read_request(fd, &req, &body, &body_len);
    if (pr == -2) {
        send_error(fd, 413, "Payload Too Large");
        free(body);
        return;
    }
    if (pr != 0) {
        send_error(fd, 400, "Bad Request");
        free(body);
        return;
    }

    if (strcmp(req.method, "GET") == 0 &&
        strcmp(req.path, "/health") == 0) {
        handle_health(fd);
    } else if (strcmp(req.method, "POST") == 0 &&
               strcmp(req.path, "/search") == 0) {
        handle_search(fd, &req, body, body_len);
    } else {
        send_error(fd, 404, "Not Found");
    }

    free(body);
}

int lisa_http_serve(int port) {
    if (port <= 0 || port > 65535) return -1;

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return -2;

    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* localhost only */
    addr.sin_port = htons((uint16_t)port);

    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sfd);
        return -3;
    }
    if (listen(sfd, 16) != 0) {
        close(sfd);
        return -4;
    }

    g_listen_fd = sfd;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    while (g_running) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                if (!g_running) break;
                continue;
            }
            break;
        }
        handle_connection(cfd);
        close(cfd);
    }

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    } else {
        close(sfd);
    }
    return 0;
}
