/**
 * @file json.c
 * @brief 最小 JSON DOM 实现（递归下降解析、线性容器、紧凑/缩进序列化）
 */
#include "core/json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

struct json {
    json_type_t type;
    union {
        bool    b;
        double  n;
        char   *s;
        struct { json_t **items; char **keys; size_t count, cap; } c;   /* array: keys==NULL */
    } u;
};

static char *xstrndup(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n); d[n] = 0;
    return d;
}

/* ------------------------------------------------------------------ */
/* 构建                                                               */
/* ------------------------------------------------------------------ */

static json_t *node_new(json_type_t t)
{
    json_t *j = (json_t *)calloc(1, sizeof(*j));
    if (j) j->type = t;
    return j;
}
json_t *json_new_null(void) { return node_new(JSON_NULL); }
json_t *json_new_bool(bool v) { json_t *j = node_new(JSON_BOOL); if (j) j->u.b = v; return j; }
json_t *json_new_number(double v) { json_t *j = node_new(JSON_NUMBER); if (j) j->u.n = v; return j; }
json_t *json_new_int(int64_t v) { return json_new_number((double)v); }
json_t *json_new_string(const char *s)
{
    json_t *j = node_new(JSON_STRING);
    if (!j) return NULL;
    j->u.s = xstrndup(s ? s : "", strlen(s ? s : ""));
    if (!j->u.s) { free(j); return NULL; }
    return j;
}
json_t *json_new_array(void) { return node_new(JSON_ARRAY); }
json_t *json_new_object(void) { return node_new(JSON_OBJECT); }

static int container_grow(json_t *c)
{
    size_t ncap = c->u.c.cap ? c->u.c.cap * 2 : 8;
    json_t **ni = (json_t **)realloc(c->u.c.items, ncap * sizeof(*ni));
    if (!ni) return -1;
    c->u.c.items = ni;
    if (c->type == JSON_OBJECT) {
        char **nk = (char **)realloc(c->u.c.keys, ncap * sizeof(*nk));
        if (!nk) return -1;
        c->u.c.keys = nk;
    }
    c->u.c.cap = ncap;
    return 0;
}

int json_array_push(json_t *arr, json_t *val)
{
    if (!arr || arr->type != JSON_ARRAY || !val) return -1;
    if (arr->u.c.count == arr->u.c.cap && container_grow(arr) != 0) return -1;
    arr->u.c.items[arr->u.c.count++] = val;
    return 0;
}

static long object_find(const json_t *obj, const char *key)
{
    for (size_t i = 0; i < obj->u.c.count; i++) {
        if (strcmp(obj->u.c.keys[i], key) == 0) return (long)i;
    }
    return -1;
}

int json_object_set(json_t *obj, const char *key, json_t *val)
{
    long idx;
    if (!obj || obj->type != JSON_OBJECT || !key || !val) return -1;
    idx = object_find(obj, key);
    if (idx >= 0) {
        json_free(obj->u.c.items[idx]);
        obj->u.c.items[idx] = val;
        return 0;
    }
    if (obj->u.c.count == obj->u.c.cap && container_grow(obj) != 0) return -1;
    obj->u.c.keys[obj->u.c.count] = xstrndup(key, strlen(key));
    if (!obj->u.c.keys[obj->u.c.count]) return -1;
    obj->u.c.items[obj->u.c.count++] = val;
    return 0;
}

json_t *json_object_remove(json_t *obj, const char *key)
{
    long idx; json_t *v;
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    idx = object_find(obj, key);
    if (idx < 0) return NULL;
    v = obj->u.c.items[idx];
    free(obj->u.c.keys[idx]);
    for (size_t i = (size_t)idx + 1; i < obj->u.c.count; i++) {
        obj->u.c.items[i - 1] = obj->u.c.items[i];
        obj->u.c.keys[i - 1] = obj->u.c.keys[i];
    }
    obj->u.c.count--;
    return v;
}

void json_free(json_t *j)
{
    if (!j) return;
    switch (j->type) {
    case JSON_STRING: free(j->u.s); break;
    case JSON_ARRAY:
    case JSON_OBJECT:
        for (size_t i = 0; i < j->u.c.count; i++) {
            json_free(j->u.c.items[i]);
            if (j->u.c.keys) free(j->u.c.keys[i]);
        }
        free(j->u.c.items);
        free(j->u.c.keys);
        break;
    default: break;
    }
    free(j);
}

json_t *json_clone(const json_t *j)
{
    json_t *c;
    if (!j) return NULL;
    switch (j->type) {
    case JSON_NULL:   return json_new_null();
    case JSON_BOOL:   return json_new_bool(j->u.b);
    case JSON_NUMBER: return json_new_number(j->u.n);
    case JSON_STRING: return json_new_string(j->u.s);
    case JSON_ARRAY:
        c = json_new_array();
        for (size_t i = 0; c && i < j->u.c.count; i++) {
            json_t *ic = json_clone(j->u.c.items[i]);
            if (!ic || json_array_push(c, ic) != 0) { json_free(ic); json_free(c); return NULL; }
        }
        return c;
    case JSON_OBJECT:
        c = json_new_object();
        for (size_t i = 0; c && i < j->u.c.count; i++) {
            json_t *ic = json_clone(j->u.c.items[i]);
            if (!ic || json_object_set(c, j->u.c.keys[i], ic) != 0) { json_free(ic); json_free(c); return NULL; }
        }
        return c;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 查询                                                               */
/* ------------------------------------------------------------------ */

json_type_t json_type(const json_t *j) { return j ? j->type : JSON_NULL; }
bool json_is(const json_t *j, json_type_t t) { return j && j->type == t; }
bool json_bool(const json_t *j, bool def) { return (j && j->type == JSON_BOOL) ? j->u.b : def; }
double json_number(const json_t *j, double def) { return (j && j->type == JSON_NUMBER) ? j->u.n : def; }
int64_t json_int(const json_t *j, int64_t def) { return (j && j->type == JSON_NUMBER) ? (int64_t)j->u.n : def; }
const char *json_string(const json_t *j, const char *def) { return (j && j->type == JSON_STRING) ? j->u.s : def; }

size_t json_size(const json_t *j)
{
    return (j && (j->type == JSON_ARRAY || j->type == JSON_OBJECT)) ? j->u.c.count : 0;
}
const json_t *json_get(const json_t *obj, const char *key)
{
    long idx;
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    idx = object_find(obj, key);
    return idx >= 0 ? obj->u.c.items[idx] : NULL;
}
const json_t *json_at(const json_t *c, size_t idx)
{
    if (!c || (c->type != JSON_ARRAY && c->type != JSON_OBJECT) || idx >= c->u.c.count) return NULL;
    return c->u.c.items[idx];
}
const char *json_key_at(const json_t *obj, size_t idx)
{
    if (!obj || obj->type != JSON_OBJECT || idx >= obj->u.c.count) return NULL;
    return obj->u.c.keys[idx];
}

const json_t *json_path(const json_t *root, const char *path)
{
    const json_t *cur = root;
    char seg[128];
    if (!root || !path) return NULL;
    while (*path && cur) {
        size_t n = 0;
        while (path[n] && path[n] != '.') n++;
        if (n == 0 || n >= sizeof(seg)) return NULL;
        memcpy(seg, path, n); seg[n] = 0;
        if (cur->type == JSON_ARRAY) {
            char *end; long idx = strtol(seg, &end, 10);
            if (*end || idx < 0) return NULL;
            cur = json_at(cur, (size_t)idx);
        } else if (cur->type == JSON_OBJECT) {
            cur = json_get(cur, seg);
        } else {
            return NULL;
        }
        path += n;
        if (*path == '.') path++;
    }
    return cur;
}

/* ------------------------------------------------------------------ */
/* 解析                                                               */
/* ------------------------------------------------------------------ */

typedef struct { const char *start, *p, *end; char *err; size_t errcap; int depth; } parser_t;

static void perr(parser_t *ps, const char *msg)
{
    if (ps->err && ps->errcap) snprintf(ps->err, ps->errcap, "%s at offset %ld", msg, (long)(ps->p - ps->start));
}
static void skip_ws(parser_t *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static json_t *parse_value(parser_t *ps);

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *parse_string_raw(parser_t *ps)
{
    /* 假定 *ps->p == '"' */
    size_t cap = 32, n = 0; char *out;
    ps->p++;
    out = (char *)malloc(cap);
    if (!out) return NULL;
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (n + 4 >= cap) { char *o2 = (char *)realloc(out, cap *= 2); if (!o2) { free(out); return NULL; } out = o2; }
        if (c == '\\') {
            if (ps->p >= ps->end) { free(out); perr(ps, "bad escape"); return NULL; }
            c = *ps->p++;
            switch (c) {
            case '"': out[n++] = '"'; break;
            case '\\': out[n++] = '\\'; break;
            case '/': out[n++] = '/'; break;
            case 'b': out[n++] = '\b'; break;
            case 'f': out[n++] = '\f'; break;
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (ps->end - ps->p < 4) { free(out); perr(ps, "bad \\u"); return NULL; }
                for (int i = 0; i < 4; i++) { int h = hexval(ps->p[i]); if (h < 0) { free(out); perr(ps, "bad \\u"); return NULL; } cp = cp * 16 + (unsigned)h; }
                ps->p += 4;
                if (cp < 0x80) out[n++] = (char)cp;
                else if (cp < 0x800) { out[n++] = (char)(0xC0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
                else { out[n++] = (char)(0xE0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
                break;
            }
            default: free(out); perr(ps, "bad escape"); return NULL;
            }
        } else {
            out[n++] = c;
        }
    }
    if (ps->p >= ps->end) { free(out); perr(ps, "unterminated string"); return NULL; }
    ps->p++; /* closing quote */
    out[n] = 0;
    return out;
}

static json_t *parse_value(parser_t *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) { perr(ps, "unexpected end"); return NULL; }
    if (++ps->depth > 64) { perr(ps, "too deep"); return NULL; }

    json_t *result = NULL;
    char c = *ps->p;
    if (c == '{') {
        json_t *obj = json_new_object();
        ps->p++;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; result = obj; goto done; }
        for (;;) {
            char *key; json_t *val;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') { json_free(obj); perr(ps, "expected key"); goto done; }
            key = parse_string_raw(ps);
            if (!key) { json_free(obj); goto done; }
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { free(key); json_free(obj); perr(ps, "expected ':'"); goto done; }
            ps->p++;
            val = parse_value(ps);
            if (!val) { free(key); json_free(obj); goto done; }
            json_object_set(obj, key, val);
            free(key);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == '}') { ps->p++; result = obj; goto done; }
            json_free(obj); perr(ps, "expected ',' or '}'"); goto done;
        }
    } else if (c == '[') {
        json_t *arr = json_new_array();
        ps->p++;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; result = arr; goto done; }
        for (;;) {
            json_t *val = parse_value(ps);
            if (!val) { json_free(arr); goto done; }
            json_array_push(arr, val);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == ']') { ps->p++; result = arr; goto done; }
            json_free(arr); perr(ps, "expected ',' or ']'"); goto done;
        }
    } else if (c == '"') {
        char *s = parse_string_raw(ps);
        if (s) { result = node_new(JSON_STRING); if (result) result->u.s = s; else free(s); }
    } else if (c == 't' && ps->end - ps->p >= 4 && strncmp(ps->p, "true", 4) == 0) {
        ps->p += 4; result = json_new_bool(true);
    } else if (c == 'f' && ps->end - ps->p >= 5 && strncmp(ps->p, "false", 5) == 0) {
        ps->p += 5; result = json_new_bool(false);
    } else if (c == 'n' && ps->end - ps->p >= 4 && strncmp(ps->p, "null", 4) == 0) {
        ps->p += 4; result = json_new_null();
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64]; size_t n = 0; char *end; double v;
        while (ps->p < ps->end && n < sizeof(buf) - 1 &&
               (*ps->p == '-' || *ps->p == '+' || *ps->p == '.' || *ps->p == 'e' || *ps->p == 'E' || (*ps->p >= '0' && *ps->p <= '9'))) {
            buf[n++] = *ps->p++;
        }
        buf[n] = 0;
        v = strtod(buf, &end);
        if (*end) { perr(ps, "bad number"); }
        else result = json_new_number(v);
    } else {
        perr(ps, "unexpected character");
    }
done:
    ps->depth--;
    return result;
}

json_t *json_parse(const char *text, size_t len, char *err, size_t errcap)
{
    parser_t ps; json_t *j;
    if (!text) return NULL;
    if (len == 0) len = strlen(text);
    ps.start = text; ps.p = text; ps.end = text + len; ps.err = err; ps.errcap = errcap; ps.depth = 0;
    if (err && errcap) err[0] = 0;
    j = parse_value(&ps);
    if (!j) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) { json_free(j); perr(&ps, "trailing characters"); return NULL; }
    return j;
}

/* ------------------------------------------------------------------ */
/* 序列化                                                             */
/* ------------------------------------------------------------------ */

typedef struct { char *buf; size_t len, cap; } sb_t;

static int sb_put(sb_t *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 256;
        while (sb->len + n + 1 > ncap) ncap *= 2;
        char *nb = (char *)realloc(sb->buf, ncap);
        if (!nb) return -1;
        sb->buf = nb; sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, s, n); sb->len += n; sb->buf[sb->len] = 0;
    return 0;
}
static int sb_puts(sb_t *sb, const char *s) { return sb_put(sb, s, strlen(s)); }
static int sb_putc(sb_t *sb, char c) { return sb_put(sb, &c, 1); }

static int dump_string(sb_t *sb, const char *s)
{
    if (sb_putc(sb, '"')) return -1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': if (sb_puts(sb, "\\\"")) return -1; break;
        case '\\': if (sb_puts(sb, "\\\\")) return -1; break;
        case '\n': if (sb_puts(sb, "\\n")) return -1; break;
        case '\r': if (sb_puts(sb, "\\r")) return -1; break;
        case '\t': if (sb_puts(sb, "\\t")) return -1; break;
        default:
            if (c < 0x20) { char tmp[8]; snprintf(tmp, sizeof(tmp), "\\u%04x", c); if (sb_puts(sb, tmp)) return -1; }
            else if (sb_putc(sb, (char)c)) return -1;
        }
    }
    return sb_putc(sb, '"');
}

static int indent(sb_t *sb, int n) { for (int i = 0; i < n; i++) if (sb_puts(sb, "  ")) return -1; return 0; }

static int dump_value(sb_t *sb, const json_t *j, bool pretty, int level)
{
    char num[32];
    if (!j) return sb_puts(sb, "null");
    switch (j->type) {
    case JSON_NULL: return sb_puts(sb, "null");
    case JSON_BOOL: return sb_puts(sb, j->u.b ? "true" : "false");
    case JSON_NUMBER:
        if (j->u.n == floor(j->u.n) && fabs(j->u.n) < 9007199254740992.0) snprintf(num, sizeof(num), "%lld", (long long)j->u.n);
        else snprintf(num, sizeof(num), "%.17g", j->u.n);
        return sb_puts(sb, num);
    case JSON_STRING: return dump_string(sb, j->u.s);
    case JSON_ARRAY:
    case JSON_OBJECT: {
        bool isobj = j->type == JSON_OBJECT;
        if (sb_putc(sb, isobj ? '{' : '[')) return -1;
        if (j->u.c.count == 0) return sb_putc(sb, isobj ? '}' : ']');
        for (size_t i = 0; i < j->u.c.count; i++) {
            if (i) { if (sb_putc(sb, ',')) return -1; }
            if (pretty) { if (sb_putc(sb, '\n') || indent(sb, level + 1)) return -1; }
            if (isobj) { if (dump_string(sb, j->u.c.keys[i]) || sb_puts(sb, pretty ? ": " : ":")) return -1; }
            if (dump_value(sb, j->u.c.items[i], pretty, level + 1)) return -1;
        }
        if (pretty) { if (sb_putc(sb, '\n') || indent(sb, level)) return -1; }
        return sb_putc(sb, isobj ? '}' : ']');
    }
    }
    return -1;
}

char *json_dump(const json_t *j, bool pretty)
{
    sb_t sb = { NULL, 0, 0 };
    if (dump_value(&sb, j, pretty, 0) != 0) { free(sb.buf); return NULL; }
    if (!sb.buf) sb_puts(&sb, "");
    return sb.buf;
}
