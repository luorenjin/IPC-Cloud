/**
 * @file http_parse.c
 * @brief HTTP/1.1 请求解析（纯函数，不碰 socket）
 */
#include "http_server.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

static int ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

const char *http_header(const http_req_t *req, const char *name)
{
    size_t i;
    if (!req || !name) return NULL;
    for (i = 0; i < req->header_count; i++) {
        if (ci_equal(req->headers[i].name, name)) return req->headers[i].value;
    }
    return NULL;
}

const char *http_query(const http_req_t *req, const char *key, char *buf, size_t cap, const char *def)
{
    const char *p;
    size_t klen;
    if (!req || !key || !buf || cap == 0) return def;
    klen = strlen(key);
    p = req->query;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = (size_t)(end - p) - klen - 1;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(buf, p + klen + 1, vlen);
            buf[vlen] = '\0';
            return buf;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return def;
}

/* URL 解码到 dst（就地安全：dst 可与 src 不同）；返回 0 成功 */
static int url_decode(char *dst, size_t cap, const char *src, size_t len)
{
    size_t di = 0, si = 0;
    while (si < len) {
        if (di + 1 >= cap) return -1;
        if (src[si] == '%' && si + 2 < len && isxdigit((unsigned char)src[si+1]) && isxdigit((unsigned char)src[si+2])) {
            char hex[3] = { src[si+1], src[si+2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 3;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
            si++;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return 0;
}

hal_err_t http_parse_request(const char *buf, size_t len, http_req_t *req, size_t *consumed)
{
    const char *hdr_end, *line, *sp1, *sp2, *eol;
    size_t head_len, body_len = 0;
    const char *cl;

    if (!buf || !req || !consumed) return HAL_EINVAL;

    /* 头部必须以空行结束 */
    hdr_end = NULL;
    if (len >= 4) {
        size_t i;
        for (i = 0; i + 3 < len; i++) {
            if (memcmp(buf + i, "\r\n\r\n", 4) == 0) { hdr_end = buf + i + 4; break; }
        }
    }
    if (!hdr_end) {
        /* 头部尚未收完；但若已明显超长则判为畸形 */
        if (len > HTTP_PATH_MAX + HTTP_QUERY_MAX + 4096) return HAL_EINVAL;
        return HAL_EAGAIN;
    }
    head_len = (size_t)(hdr_end - buf);

    memset(req, 0, sizeof(*req));

    /* 请求行：METHOD SP TARGET SP VERSION */
    line = buf;
    eol = memchr(line, '\r', head_len);
    if (!eol) return HAL_EINVAL;
    sp1 = memchr(line, ' ', (size_t)(eol - line));
    if (!sp1) return HAL_EINVAL;
    if ((size_t)(sp1 - line) >= sizeof(req->method)) return HAL_EINVAL;
    memcpy(req->method, line, (size_t)(sp1 - line));
    req->method[sp1 - line] = '\0';

    sp2 = memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
    if (!sp2) return HAL_EINVAL;
    {
        const char *target = sp1 + 1;
        size_t tlen = (size_t)(sp2 - target);
        const char *q = memchr(target, '?', tlen);
        size_t plen = q ? (size_t)(q - target) : tlen;
        size_t qlen = q ? tlen - plen - 1 : 0;

        if (plen == 0 || plen >= HTTP_PATH_MAX) return HAL_EINVAL;
        if (qlen >= HTTP_QUERY_MAX) return HAL_EINVAL;
        if (url_decode(req->path, sizeof(req->path), target, plen) != 0) return HAL_EINVAL;
        if (qlen) {
            memcpy(req->query, q + 1, qlen);
            req->query[qlen] = '\0';
        }
    }

    /* 头字段：就地切分需要可写缓冲，这里改为记录指针 + 长度受限的静态解析
       为保持 buf 只读，headers 指向 buf 内部并依赖后续 NUL 化由调用者完成。
       简化实现：拷贝头部区到内部静态缓冲。 */
    {
        static char hbuf[8192];
        char *p, *end;
        if (head_len >= sizeof(hbuf)) return HAL_EINVAL;
        memcpy(hbuf, buf, head_len);
        hbuf[head_len] = '\0';

        p = strstr(hbuf, "\r\n");
        if (!p) return HAL_EINVAL;
        p += 2;
        end = hbuf + head_len;
        while (p < end && req->header_count < HTTP_HEADERS_MAX) {
            char *colon, *lineend;
            if (p[0] == '\r' && p[1] == '\n') break;   /* 头结束 */
            lineend = strstr(p, "\r\n");
            if (!lineend) break;
            *lineend = '\0';
            colon = strchr(p, ':');
            if (colon) {
                *colon = '\0';
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                req->headers[req->header_count].name = p;
                req->headers[req->header_count].value = colon;
                req->header_count++;
            }
            p = lineend + 2;
        }
    }

    /* body */
    cl = http_header(req, "Content-Length");
    if (cl) {
        long v = strtol(cl, NULL, 10);
        if (v < 0 || v > HTTP_BODY_MAX) return HAL_EINVAL;
        body_len = (size_t)v;
    }
    if (len < head_len + body_len) return HAL_EAGAIN;

    req->body = body_len ? (buf + head_len) : NULL;
    req->body_len = body_len;
    *consumed = head_len + body_len;
    return HAL_OK;
}
