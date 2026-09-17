/*
 * GTA6 Blog - pure C HTTP backend + FlashDB database
 * https://github.com/armink/FlashDB.git (vendored in third_party/)
 *
 * Features:
 *  - No web framework, only POSIX sockets + pthreads + FlashDB
 *  - KVDB  : posts, comments, newsletter, sessions, counters
 *  - TSDB  : page-view / event log (time series)
 *  - Admin : user `kono` / pass `kono`, cookie session
 *  - JSON REST API + static file server (public/, Bootstrap 5 via CDN)
 *
 * Build: make
 * Run  : ./gta6-blog [port]   (default 8080, $PORT wins)
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "flashdb.h"

#define ADMIN_USER "kono"
#define ADMIN_PASS "kono"

#define MAX_POST_SIZE   (24*1024)
#define MAX_BODY        (64*1024)
#define MAX_REQ         (96*1024)
#define MAX_RESP_HEAD   4096
#define BLOB_BUF        (32*1024)

static struct fdb_kvdb g_kvdb;
static struct fdb_tsdb g_tsdb;
static pthread_mutex_t g_kv_locker;
static pthread_mutex_t g_ts_locker;
static pthread_mutex_t g_db_api_locker = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_token_locker  = PTHREAD_MUTEX_INITIALIZER;
static char g_admin_token[64] = {0};
static int  g_port = 8080;
static const char *g_public_dir = "public";

/* ---------------- FlashDB lock callbacks ---------------- */
static void kv_lock(fdb_db_t db)   { (void)db; pthread_mutex_lock(&g_kv_locker); }
static void kv_unlock(fdb_db_t db) { (void)db; pthread_mutex_unlock(&g_kv_locker); }
static void ts_lock(fdb_db_t db)   { (void)db; pthread_mutex_lock(&g_ts_locker); }
static void ts_unlock(fdb_db_t db) { (void)db; pthread_mutex_unlock(&g_ts_locker); }
static fdb_time_t blog_time(void)  { return (fdb_time_t) time(NULL); }

/* ---------------- small utils ---------------- */
static void url_decode(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '+') { *d++ = ' '; s++; }
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *d++ = (char) strtol(h, NULL, 16);
            s += 3;
        } else *d++ = *s++;
    }
    *d = 0;
}

static void json_escape_into(const char *src, char *dst, size_t dstsz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dstsz; i++) {
        unsigned char c = (unsigned char) src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dstsz) break;
            dst[j++] = '\\'; dst[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 2 >= dstsz) break;
            dst[j++] = '\\'; dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= dstsz) break;
            dst[j++] = '\\'; dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= dstsz) break;
            dst[j++] = '\\'; dst[j++] = 't';
        } else if (c < 0x20) {
            /* skip controls */
        } else dst[j++] = (char)c;
    }
    dst[j] = 0;
}

/* Extract "key" : "string value" (with basic unescape) or bare token.
 * Returns 1 on success. */
static int json_field(const char *json, const char *key, char *out, size_t outsz) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && (isspace((unsigned char)*p) || *p == ':')) {
        if (*p == ':') { p++; break; }
        p++;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '"') {
        p++;
        size_t j = 0;
        while (*p && *p != '"' && j + 1 < outsz) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') out[j++] = '\n';
                else if (*p == 'r') out[j++] = '\r';
                else if (*p == 't') out[j++] = '\t';
                else out[j++] = *p;
                p++;
            } else out[j++] = *p++;
        }
        out[j] = 0;
        return 1;
    } else {
        size_t j = 0;
        while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p) && j + 1 < outsz)
            out[j++] = *p++;
        out[j] = 0;
        return j > 0;
    }
}

static int json_field_int(const char *json, const char *key, int dflt) {
    char tmp[64];
    if (!json_field(json, key, tmp, sizeof(tmp))) return dflt;
    return atoi(tmp);
}

/* query string: hay="a=1&b=2" -> value of key into out */
static int query_param(const char *qs, const char *key, char *out, size_t outsz) {
    if (!qs || !*qs) return 0;
    size_t kl = strlen(key);
    const char *p = qs;
    while (*p) {
        if ((p == qs || p[-1] == '&' || p[-1] == '?') &&
            strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t j = 0;
            while (*v && *v != '&' && j + 1 < outsz) out[j++] = *v++;
            out[j] = 0;
            url_decode(out);
            return 1;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return 0;
}

static int cookie_param(const char *cookie_hdr, const char *key, char *out, size_t outsz) {
    if (!cookie_hdr) return 0;
    size_t kl = strlen(key);
    const char *p = cookie_hdr;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t j = 0;
            while (*v && *v != ';' && *v != '\r' && *v != '\n' && j + 1 < outsz) out[j++] = *v++;
            out[j] = 0;
            return 1;
        }
        while (*p && *p != ';') p++;
    }
    return 0;
}

static const char *mime_for(const char *path) {
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    if (!strcasecmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcasecmp(e, ".css"))  return "text/css; charset=utf-8";
    if (!strcasecmp(e, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcasecmp(e, ".json")) return "application/json; charset=utf-8";
    if (!strcasecmp(e, ".png"))  return "image/png";
    if (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(e, ".webp")) return "image/webp";
    if (!strcasecmp(e, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(e, ".ico"))  return "image/x-icon";
    if (!strcasecmp(e, ".woff2"))return "font/woff2";
    return "application/octet-stream";
}

/* ---------------- FlashDB KV helpers (blob-safe, >128B OK) ---------------- */
static char *db_get(const char *key) {
    pthread_mutex_lock(&g_db_api_locker);
    char *buf = malloc(BLOB_BUF);
    if (!buf) { pthread_mutex_unlock(&g_db_api_locker); return NULL; }
    struct fdb_blob blob;
    size_t n = fdb_kv_get_blob(&g_kvdb, key,
                fdb_blob_make(&blob, buf, BLOB_BUF - 1));
    char *out = NULL;
    if (n > 0) {
        size_t total = blob.saved.len ? blob.saved.len : n;
        if (total >= BLOB_BUF) total = BLOB_BUF - 1;
        /* if value bigger than buffer, n is truncated; still usable up to buffer */
        buf[n] = '\0';
        (void)total;
        out = strdup(buf);
    } else if (blob.saved.len > 0) {
        /* value exists but larger than buffer: read in chunks? fallback: retry bigger */
        free(buf);
        size_t big = blob.saved.len + 1;
        if (big > (size_t)(256*1024)) big = 256*1024;
        buf = malloc(big);
        if (buf) {
            struct fdb_blob b2;
            n = fdb_kv_get_blob(&g_kvdb, key, fdb_blob_make(&b2, buf, big - 1));
            if (n > 0) { buf[n] = '\0'; out = strdup(buf); }
            free(buf);
            pthread_mutex_unlock(&g_db_api_locker);
            return out;
        }
        pthread_mutex_unlock(&g_db_api_locker);
        return NULL;
    }
    free(buf);
    pthread_mutex_unlock(&g_db_api_locker);
    return out; /* NULL = missing */
}

static int db_set(const char *key, const char *val) {
    pthread_mutex_lock(&g_db_api_locker);
    fdb_err_t r = fdb_kv_set(&g_kvdb, key, val);
    pthread_mutex_unlock(&g_db_api_locker);
    return r == FDB_NO_ERR;
}

static int db_del(const char *key) {
    pthread_mutex_lock(&g_db_api_locker);
    fdb_err_t r = fdb_kv_del(&g_kvdb, key);
    pthread_mutex_unlock(&g_db_api_locker);
    return r == FDB_NO_ERR;
}

static long db_get_long(const char *key, long dflt) {
    char *v = db_get(key);
    if (!v) return dflt;
    long x = atol(v);
    free(v);
    return x;
}

/* atomically increment an integer KV, returns new value */
static long db_incr(const char *key) {
    long v = db_get_long(key, 0) + 1;
    char s[32]; snprintf(s, sizeof(s), "%ld", v);
    db_set(key, s);
    return v;
}

/* forward declarations (defined further below) */
static void tsdb_log(const char *event);
static void today_iso(char *out, size_t n);

/* keep cc as 2 uppercase letters, else "XX" (unknown) */
static void sanitize_cc(const char *in, char out[3]) {
    if (in && isalpha((unsigned char)in[0]) && isalpha((unsigned char)in[1]) &&
        (in[2] == 0 || in[2] == '\0')) {
        out[0] = toupper((unsigned char)in[0]);
        out[1] = toupper((unsigned char)in[1]);
        out[2] = 0;
    } else strcpy(out, "XX");
}

/* keep a short printable country name, else "Unknown" */
static void sanitize_country(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    if (in) for (size_t i = 0; in[i] && j + 1 < outsz && j < 48; i++) {
        char c = in[i];
        if (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '\'' || c == '.')
            out[j++] = c;
    }
    out[j] = 0;
    /* trim */
    while (j && out[j-1] == ' ') out[--j] = 0;
    if (j < 2) strcpy(out, "Unknown");
}

/* privacy: 1.2.3.4 -> 1.2.3.*  ;  ::1 -> ::1 (loopback kept) */
static void mask_ip(const char *ip, char *out, size_t outsz) {
    if (!ip || !*ip) { strncpy(out, "-", outsz - 1); out[outsz-1] = 0; return; }
    const char *dot = strrchr(ip, '.');
    const char *colon = strrchr(ip, ':');
    if (dot && (!colon || dot > colon)) {
        size_t n = (size_t)(dot - ip);
        if (n + 2 >= outsz) n = outsz - 3;
        memcpy(out, ip, n); out[n] = 0; strcat(out, ".*");
    } else {
        strncpy(out, ip, outsz - 1); out[outsz-1] = 0;
    }
}

/* append item to a comma-separated index list KV (dedupe, cap entries) */
static void idx_add(const char *listkey, const char *item, int cap) {
    char *cur = db_get(listkey);
    if (!cur) cur = strdup("");
    /* already present? */
    {
        char *cp = strdup(cur);
        for (char *t = strtok(cp, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!strcmp(t, item)) { free(cp); free(cur); return; }
        }
        free(cp);
    }
    size_t need = strlen(cur) + strlen(item) + 2;
    char *next = malloc(need);
    snprintf(next, need, "%s%s%s", cur, *cur ? "," : "", item);
    free(cur);
    /* enforce cap: drop oldest (front) entries */
    int n = 1; for (char *p = next; *p; p++) if (*p == ',') n++;
    while (n > cap) {
        char *c = strchr(next, ',');
        if (!c) break;
        memmove(next, c + 1, strlen(c + 1) + 1);
        n--;
    }
    db_set(listkey, next);
    free(next);
}

/* push one JSON object (already escaped) onto the viewlog ring KV (cap 60) */
static void viewlog_push(const char *obj) {
    char *cur = db_get("viewlog");
    char *next;
    if (!cur || !strcmp(cur, "[]") || !cur[0]) {
        next = malloc(strlen(obj) + 8);
        sprintf(next, "[%s]", obj);
    } else {
        /* count entries; drop oldest while >= 60 */
        int n = 0;
        for (char *p = cur; *p; p++) if (!strncmp(p, "\"t\":", 4)) n++;
        while (n >= 60) {
            /* remove first top-level {...} */
            char *st = strchr(cur, '{');
            if (!st) break;
            int depth = 0, instr = 0;
            char *p = st;
            while (*p) {
                if (*p == '"' && (p == st || p[-1] != '\\')) instr = !instr;
                if (!instr) {
                    if (*p == '{') depth++;
                    else if (*p == '}') { depth--; if (!depth) { p++; break; } }
                }
                p++;
            }
            while (*p == ',' || *p == ' ' ) p++;
            memmove(st, p, strlen(p) + 1);
            n--;
            if (*st == ']') break;
        }
        cur[strlen(cur)-1] = 0; /* strip ] */
        next = malloc(strlen(cur) + strlen(obj) + 8);
        sprintf(next, "%s%s%s]", cur, cur[strlen(cur)-1] == '[' ? "" : ",", obj);
    }
    if (cur) free(cur);
    db_set("viewlog", next);
    free(next);
}

/* central visit tracker: post page (is_post=1, pid=post id) or site page (pid="home"/"about").
 * Returns updated views_<pid> when is_post, else total site views. */
static long track_view(const char *pid, int is_post, const char *ip, const char *cc, const char *cname) {
    long pv = 0;
    if (is_post) {
        char vk[64]; snprintf(vk, sizeof(vk), "views_%s", pid);
        pv = db_incr(vk);
    }
    long sv = db_incr("site_views");
    char date[16]; today_iso(date, sizeof(date));
    char dk[32]; snprintf(dk, sizeof(dk), "daily_%s", date);
    db_incr(dk);
    idx_add("daily_index", date, 90);
    /* country */
    char ck[16]; snprintf(ck, sizeof(ck), "ctr_%s", cc);
    db_incr(ck);
    idx_add("ctr_index", cc, 250);
    char nk[24]; snprintf(nk, sizeof(nk), "ctrname_%s", cc);
    char *old = db_get(nk);
    if (!old) db_set(nk, cname);
    else free(old);
    /* recent ring */
    char masked[80]; mask_ip(ip, masked, sizeof(masked));
    char en[64], ec2[128];
    json_escape_into(masked, en, sizeof(en));
    char cn_e[128]; json_escape_into(cname, cn_e, sizeof(cn_e));
    char obj[512];
    snprintf(obj, sizeof(obj), "{\"t\":%ld,\"p\":\"%s\",\"cc\":\"%s\",\"n\":\"%s\",\"ip\":\"%s\"}",
             (long)time(NULL), pid, cc, cn_e, en);
    (void)ec2;
    viewlog_push(obj);
    /* TSDB event log */
    char ev[320];
    snprintf(ev, sizeof(ev), "view:%s:%s:%s:%ld", pid, masked, cc, (long)time(NULL));
    tsdb_log(ev);
    return is_post ? pv : sv;
}

static void tsdb_log(const char *event) {
    if (!event || !*event) return;
    pthread_mutex_lock(&g_db_api_locker);
    struct fdb_blob b;
    fdb_tsl_append(&g_tsdb, fdb_blob_make(&b, event, strlen(event)));
    pthread_mutex_unlock(&g_db_api_locker);
}

/* ---------------- auth ---------------- */
static void make_token(char *out, size_t n) {
    static const char *H = "0123456789abcdef";
    unsigned int s = (unsigned int)(time(NULL) ^ getpid() ^ rand());
    srand(s + (unsigned int)rand());
    for (size_t i = 0; i + 1 < n; i++) out[i] = H[rand() % 16];
    out[n-1] = 0;
}

static int is_admin(const char *cookie_hdr) {
    char tok[128] = {0};
    if (!cookie_param(cookie_hdr, "session", tok, sizeof(tok))) return 0;
    int ok = 0;
    pthread_mutex_lock(&g_token_locker);
    if (g_admin_token[0] && strcmp(tok, g_admin_token) == 0) ok = 1;
    pthread_mutex_unlock(&g_token_locker);
    return ok;
}

/* ---------------- blog model ----------------
 * KV layout (FlashDB KVDB):
 *   posts_index      "5,4,3,2,1" (newest first)
 *   seq_post         next numeric id
 *   post_<id>        JSON object (no views inside)
 *   views_<id>       integer string (per-post visits)
 *   site_views       integer string (ALL page visits)
 *   daily_YYYY-MM-DD integer string (visits per day)
 *   daily_index      comma list of dates (last 90)
 *   ctr_<CC>         integer string (visits per country code)
 *   ctrname_<CC>     country display name
 *   ctr_index        comma list of country codes
 *   viewlog          JSON array ring (last 60 visits: t,p,cc,n,ip*)
 *   comments_<pid>   JSON array string
 *   newsletter_list  emails separated by \n
 *   admin_session    token
 *   seeded           "1"
 * TSDB "views": one record per visit, e.g. "view:3:1.2.3.*:US:1726..."
 */
static void today_iso(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, n, "%Y-%m-%d", &tmv);
}

/* YouTube video IDs look like [A-Za-z0-9_-]{6,16} (Shorts/legacy can be short) */
static int valid_video(const char *v) {
    if (!v || !*v) return 1; /* empty = no video, fine */
    size_t n = strlen(v);
    if (n < 6 || n > 16) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') return 0;
    }
    return 1;
}

static void seed_post(const char *id, const char *title, const char *cat,
                      const char *excerpt, const char *image, const char *tags,
                      const char *author, int featured, const char *video,
                      const char *content); /* fwd: defined after slug helpers */

/* Turn a title into a URL slug: lowercase, alnum kept, runs of other
 * chars become a single '-', cut at a word boundary to fit FlashDB's
 * 64-char key limit (slug_ prefix eats 5, so <= 56 chars incl. suffix room).
 * Non-ASCII bytes are treated as separators. Never empty (falls back). */
#define SLUG_MAX 48
static void make_slug(const char *title, const char *id, char *out, size_t n) {
    char tmp[128]; size_t o = 0;
    int dash = 1; /* pretend we just emitted a dash: trims leading seps */
    for (const unsigned char *p = (const unsigned char *)title; *p && o + 1 < sizeof(tmp); p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            tmp[o++] = (char)c; dash = 0;
        } else if (!dash) {
            tmp[o++] = '-'; dash = 1;
        }
    }
    while (o > 0 && tmp[o-1] == '-') o--; /* trim trailing dash */
    tmp[o] = 0;
    if (!o) { snprintf(out, n, "post-%s", id); return; }
    if (o <= SLUG_MAX) { strncpy(out, tmp, n - 1); out[n-1] = 0; return; }
    /* cut at last '-' within limit so we don't split a word */
    size_t cut = SLUG_MAX;
    for (size_t i = SLUG_MAX; i > 12; i--)
        if (tmp[i] == '-') { cut = i; break; }
    tmp[cut] = 0;
    strncpy(out, tmp, n - 1); out[n-1] = 0;
}

/* Make slug unique via slug_<slug> -> id mapping. exclude_id (may be NULL)
 * is allowed to own the slug (for edits). Result in out. */
static void slug_unique(const char *base, const char *exclude_id, char *out, size_t n) {
    strncpy(out, base, n - 1); out[n-1] = 0;
    char key[192], *owner;
    for (int i = 1; i < 1000; i++) {
        if (i > 1) snprintf(out, n, "%s-%d", base, i);
        snprintf(key, sizeof(key), "slug_%s", out);
        owner = db_get(key);
        if (!owner) return; /* free */
        int mine = exclude_id && !strcmp(owner, exclude_id);
        free(owner);
        if (mine) return;
    }
    /* absurd fallback: base + timestamp-ish suffix */
    snprintf(out, n, "%s-%ld", base, (long)time(NULL) % 100000);
}

/* Resolve slug -> numeric id. Returns 1 on success. */
static int post_id_by_slug(const char *slug, char *id_out, size_t n) {
    if (!slug || !*slug) return 0;
    for (const char *p = slug; *p; p++) {
        char c = *p;
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') return 0;
    }
    char key[192]; snprintf(key, sizeof(key), "slug_%s", slug);
    char *id = db_get(key);
    if (!id) return 0;
    strncpy(id_out, id, n - 1); id_out[n-1] = 0;
    free(id);
    return id_out[0] != 0;
}

static void seed_post(const char *id, const char *title, const char *cat,
                      const char *excerpt, const char *image, const char *tags,
                      const char *author, int featured, const char *video,
                      const char *content) {
    char date[16]; today_iso(date, sizeof(date));
    char base[96]; make_slug(title, id, base, sizeof(base));
    char slug[128]; slug_unique(base, id, slug, sizeof(slug));
    char *json = malloc(MAX_POST_SIZE);
    if (!json) return;
    char et[4096], ee[8192], ec[16000], eg[1024], ei[1024], ea[256], ev[32], es[192];
    json_escape_into(title, et, sizeof(et));
    json_escape_into(excerpt, ee, sizeof(ee));
    json_escape_into(content, ec, sizeof(ec));
    json_escape_into(tags, eg, sizeof(eg));
    json_escape_into(image, ei, sizeof(ei));
    json_escape_into(author, ea, sizeof(ea));
    json_escape_into(video ? video : "", ev, sizeof(ev));
    json_escape_into(slug, es, sizeof(es));
    char cat_e[256]; json_escape_into(cat, cat_e, sizeof(cat_e));
    snprintf(json, MAX_POST_SIZE,
        "{\"id\":\"%s\",\"slug\":\"%s\",\"title\":\"%s\",\"category\":\"%s\",\"excerpt\":\"%s\","
        "\"image\":\"%s\",\"tags\":\"%s\",\"author\":\"%s\",\"date\":\"%s\",\"featured\":%d,"
        "\"video\":\"%s\",\"content\":\"%s\"}",
        id, es, et, cat_e, ee, ei, eg, ea, date, featured, ev, ec);
    char key[64]; snprintf(key, sizeof(key), "post_%s", id);
    db_set(key, json);
    free(json);
    char skey[192]; snprintf(skey, sizeof(skey), "slug_%s", slug);
    db_set(skey, id);
}

/* v3 content pass: official videos + official info ONLY (old rumor/guide posts removed) */
static void ensure_seed(void) {
    char *s = db_get("seeded_v3");
    if (s) { free(s); return; }
    /* wipe previous seed generation (posts + their counters/comments/slugs) */
    char *old_idx = db_get("posts_index");
    if (old_idx) {
        char *cp = strdup(old_idx);
        for (char *t = strtok(cp, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!*t) continue;
            char k[80];
            snprintf(k, sizeof(k), "post_%s", t);
            char *pj = db_get(k);
            if (pj) { /* drop this post's slug mapping too */
                char sl[192] = {0};
                json_field(pj, "slug", sl, sizeof(sl));
                if (sl[0]) { char sk[256]; snprintf(sk, sizeof(sk), "slug_%s", sl); db_del(sk); }
                free(pj);
            }
            db_del(k);
            snprintf(k, sizeof(k), "views_%s", t);    db_del(k);
            snprintf(k, sizeof(k), "comments_%s", t); db_del(k);
        }
        free(cp); free(old_idx);
    }
    db_set("seq_post", "5");
    db_set("posts_index", "1,2,3,4");
    if (db_get_long("site_views", -1) < 0) db_set("site_views", "0");

    seed_post("1", "GTA VI: An Extended Look — 26 Minutes of PS5 Gameplay",
        "Gameplay",
        "Rockstar's 26-minute Extended Look: pure PS5 gameplay with Lucia & Jason across Leonida. Watch it here.",
        "https://i.ytimg.com/vi/uphThaa97ig/maxresdefault.jpg",
        "extended look, gameplay, ps5, official", "Kono", 1, "uphThaa97ig",
        "After premiering on Netflix (Aug 27, 2026), the full ~26-minute Extended Look went up on YouTube — captured entirely from in-game PS5 footage, per Rockstar. Embedded here via Netflix's official upload (same footage).\\n\\nWhat it shows:\\n- Lucia Caminos & Jason Duval story slices across Leonida\\n- Vice City, the Keys, swamps and highways\\n- Playable shootouts, police chases, basketball, scuba diving (per The Verge)\\n- Zero narration — pure atmosphere\\n\\nRelease: November 19 on PS5 + Xbox Series X|S at $79.99 (per The Verge).");

    seed_post("2", "GTA VI Trailer 2: Lucia & Jason's Story",
        "Trailers",
        "Trailer 2 goes deep on story: a Bonnie-and-Clyde crime romance across the neon state of Leonida.",
        "https://i.ytimg.com/vi/VQRLujxTm3c/maxresdefault.jpg",
        "trailer 2, lucia, jason, official", "Kono", 1, "VQRLujxTm3c",
        "Trailer 2 (May 2025) is the story trailer: Lucia fresh out of prison, Jason drifting through dead-end jobs — until one score pulls them into a statewide conspiracy.\\n\\nWhat Rockstar showed:\\n- Dual protagonists Lucia Caminos & Jason Duval\\n- Vice Beach, the Leonida Keys and countryside\\n- In-world social-media clips woven through the trailer\\n- Ocean Drive neon, visible from every angle\\n\\nWatch it above, then read the official info post for release facts audiences can trust — everything else online is unconfirmed.");

    seed_post("3", "GTA VI Trailer 1: Welcome Back to Vice City",
        "Trailers",
        "The trailer that broke the internet: 93M views in 24h and our first look at Leonida.",
        "https://i.ytimg.com/vi/QdBZY2fkU-0/maxresdefault.jpg",
        "trailer 1, vice city, official", "Kono", 0, "QdBZY2fkU-0",
        "December 2023: Trailer 1 dropped early after a leak and instantly became the most-viewed non-music video launch on YouTube (~93M in 24h).\\n\\nWhat it confirmed:\\n- Return to Vice City, state of Leonida\\n- Lucia Caminos — GTA's first female lead\\n- Tom Petty's 'Love Is a Long Road'\\n- Social-media satire baked into the world\\n\\nThe moment the hype era began. Watch the original above.");

    seed_post("4", "GTA VI Official Info: Release Date, Price, Platforms",
        "Official",
        "Only confirmed facts: November 19 launch, $79.99, PS5 + Xbox Series X|S, pre-orders live.",
        "https://images.unsplash.com/photo-1493514789931-586cb221d7a7?w=1200&q=80",
        "release date, price, platforms, official", "Kono", 0, "",
        "Everything here is straight from Rockstar — no rumors:\\n\\n- RELEASE: November 19\\n- PRICE: from $79.99\\n- PLATFORMS: PlayStation 5 + Xbox Series X|S\\n- PRE-ORDER: rockstargames.com/VI\\n- SETTING: Vice City, state of Leonida\\n- LEADS: Lucia Caminos & Jason Duval\\n- FOOTAGE: Extended Look captured entirely on PS5\\n\\nAnything else you read online (PC date, map size, DLC) is unconfirmed until Rockstar says so.");
    db_set("seeded_v3", "1");
}

/* One-time backfill: give every existing post a slug (for DBs created
 * before slugs existed). Idempotent via the slugs_v2 marker. Also heals
 * over-long slugs from the first backfill pass (v1 allowed 80 chars,
 * but FlashDB keys cap at 64). */
static void ensure_slugs(void) {
    char *m = db_get("slugs_v2");
    if (m) { free(m); return; }
    char *idx = db_get("posts_index");
    if (!idx) { db_set("slugs_v2", "1"); return; }
    char *cp = strdup(idx); free(idx);
    for (char *t = strtok(cp, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        if (!*t) continue;
        char key[80]; snprintf(key, sizeof(key), "post_%s", t);
        char *pj = db_get(key);
        if (!pj) continue;
        char sl[192] = {0};
        json_field(pj, "slug", sl, sizeof(sl));
        if (sl[0] && strlen(sl) <= SLUG_MAX + 8) { /* already good: ensure mapping */
            char sk[256]; snprintf(sk, sizeof(sk), "slug_%s", sl);
            char *owner = db_get(sk);
            if (!owner) db_set(sk, t);
            else free(owner);
            free(pj);
            continue;
        }
        if (sl[0]) { /* over-long v1 slug: drop its (broken) mapping */
            char sk[256]; snprintf(sk, sizeof(sk), "slug_%s", sl); db_del(sk);
        }
        char title[4096] = {0};
        json_field(pj, "title", title, sizeof(title));
        char base[96]; make_slug(title[0] ? title : t, t, base, sizeof(base));
        char slug[128]; slug_unique(base, t, slug, sizeof(slug));
        /* splice "slug" right after "id" field (or replace over-long one) */
        char idpat[128]; snprintf(idpat, sizeof(idpat), "\"id\":\"%s\"", t);
        char *at = strstr(pj, idpat);
        char *upd = NULL;
        if (sl[0]) { /* replace existing slug value in place */
            char spat[256]; snprintf(spat, sizeof(spat), "\"slug\":\"%s\"", sl);
            char *st = strstr(pj, spat);
            if (st) {
                upd = malloc(strlen(pj) + strlen(slug) + 8);
                size_t pre = (size_t)(st - pj);
                memcpy(upd, pj, pre);
                sprintf(upd + pre, "\"slug\":\"%s\"%s", slug, st + strlen(spat));
            }
        }
        if (!upd && at) {
            size_t pre = (size_t)(at - pj) + strlen(idpat);
            upd = malloc(strlen(pj) + strlen(slug) + 16);
            memcpy(upd, pj, pre);
            sprintf(upd + pre, ",\"slug\":\"%s\"%s", slug, pj + pre);
        }
        if (!upd) upd = strdup(pj); /* unexpected shape: leave JSON alone */
        else {
            db_set(key, upd);
            char sk[256]; snprintf(sk, sizeof(sk), "slug_%s", slug);
            db_set(sk, t);
            fprintf(stderr, "[slugs] backfilled post %s -> %s\n", t, slug);
        }
        free(upd); free(pj);
    }
    free(cp);
    db_set("slugs_v2", "1");
}

/* Build {"posts":[...], "total":N} with optional search/category filter + pagination.
 * Returns malloc'd string (caller free). */
static char *api_list_posts(const char *search, const char *cat, int page, int per) {
    char *idx = db_get("posts_index");
    if (!idx) idx = strdup("");
    /* split ids */
    char *ids[512]; int n = 0;
    char *tmp = strdup(idx); free(idx);
    for (char *t = strtok(tmp, ","); t && n < 512; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        if (*t) ids[n++] = t;
    }
    /* filter */
    struct { char *id; char *json; } keep[512]; int k = 0;
    char slow[256] = {0}, clow[128] = {0};
    if (search) { strncpy(slow, search, sizeof(slow)-1); for (char *p = slow; *p; p++) *p = tolower((unsigned char)*p); }
    if (cat)    { strncpy(clow, cat, sizeof(clow)-1);     for (char *p = clow; *p; p++) *p = tolower((unsigned char)*p); }
    for (int i = 0; i < n; i++) {
        char key[64]; snprintf(key, sizeof(key), "post_%s", ids[i]);
        char *pj = db_get(key);
        if (!pj) continue;
        int ok = 1;
        if ((search && *search) || (cat && *cat && strcmp(cat, "All") != 0)) {
            char t[4096], c[256], e[8192], g[1024];
            json_field(pj, "title", t, sizeof(t));
            json_field(pj, "category", c, sizeof(c));
            json_field(pj, "excerpt", e, sizeof(e));
            json_field(pj, "tags", g, sizeof(g));
            char hay[14000]; snprintf(hay, sizeof(hay), "%s %s %s %s", t, c, e, g);
            for (char *p = hay; *p; p++) *p = tolower((unsigned char)*p);
            if (search && *search && !strstr(hay, slow)) ok = 0;
            if (cat && *cat && strcmp(cat, "All") != 0) {
                char cc[256]; strncpy(cc, c, sizeof(cc)-1);
                for (char *p = cc; *p; p++) *p = tolower((unsigned char)*p);
                if (!strstr(cc, clow)) ok = 0;
            }
        }
        if (ok && k < 512) { keep[k].id = ids[i]; keep[k].json = pj; k++; }
        else free(pj);
    }
    free(tmp);
    if (per <= 0 || per > 50) per = 12;
    if (page <= 0) page = 1;
    int start = (page-1)*per, end = start + per;
    if (start > k) start = k;
    if (end > k) end = k;
    size_t cap = 1024 + (size_t)(end-start) * (MAX_POST_SIZE + 256);
    char *out = malloc(cap);
    size_t o = 0;
    o += snprintf(out+o, cap-o, "{\"posts\":[");
    for (int i = start; i < end; i++) {
        long views = db_get_long((char[]){0}, 0); /* noop to keep helper used */
        (void)views;
        char vk[64]; snprintf(vk, sizeof(vk), "views_%s", keep[i].id);
        long v = db_get_long(vk, 0);
        /* inject views + commentCount into post json: strip trailing } and append */
        size_t L = strlen(keep[i].json);
        char *cc_raw = NULL;
        { char ck[64]; snprintf(ck, sizeof(ck), "comments_%s", keep[i].id);
          cc_raw = db_get(ck); }
        int cc = 0;
        if (cc_raw) { for (char *p = cc_raw; *p; p++) if (strncmp(p, "\"author\"", 8) == 0) cc++; free(cc_raw); }
        if (L && keep[i].json[L-1] == '}') keep[i].json[L-1] = 0;
        o += snprintf(out+o, cap-o, "%s%s,\"views\":%ld,\"comments\":%d}",
                      i > start ? "," : "", keep[i].json, v, cc);
    }
    o += snprintf(out+o, cap-o, "],\"total\":%d,\"page\":%d,\"per\":%d}", k, page, per);
    for (int i = 0; i < k; i++) free(keep[i].json);
    return out;
}

/* single post JSON (with views+comments merged). NULL if missing. Increments views when inc!=0 */
static char *api_get_post(const char *id, int inc, const char *client_ip,
                          const char *cc, const char *cname) {
    char key[64]; snprintf(key, sizeof(key), "post_%s", id);
    char *pj = db_get(key);
    if (!pj) return NULL;
    char vk[64]; snprintf(vk, sizeof(vk), "views_%s", id);
    long v = db_get_long(vk, 0);
    if (inc) {
        v = track_view(id, 1, client_ip, cc, cname);
    }
    char ck[64]; snprintf(ck, sizeof(ck), "comments_%s", id);
    char *craw = db_get(ck);
    if (!craw) craw = strdup("[]");
    size_t L = strlen(pj);
    if (L && pj[L-1] == '}') pj[L-1] = 0;
    size_t cap = strlen(pj) + strlen(craw) + 128;
    char *out = malloc(cap);
    snprintf(out, cap, "%s,\"views\":%ld,\"commentList\":%s}", pj, v, craw);
    free(pj); free(craw);
    return out;
}

/* growable string buffer for analytics JSON */
struct sbuf { char *s; size_t len, cap; };
static void sb_add(struct sbuf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        size_t avail = b->cap - b->len;
        int w = vsnprintf(b->s + b->len, avail, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if ((size_t)w < avail) { b->len += (size_t)w; return; }
        size_t ncap = b->cap * 2 + (size_t)w + 64;
        char *ns = realloc(b->s, ncap);
        if (!ns) return;
        b->s = ns; b->cap = ncap;
    }
}

/* full analytics payload (admin). Caller frees. */
static char *api_analytics(void) {
    struct sbuf b = { malloc(8192), 0, 8192 };
    if (!b.s) return strdup("{}");
    long total = db_get_long("site_views", 0);
    char today[16]; today_iso(today, sizeof(today));
    char tdk[32]; snprintf(tdk, sizeof(tdk), "daily_%s", today);
    long today_v = db_get_long(tdk, 0);

    /* ---- per-post table ---- */
    struct poststat { char id[32], title[512], cat[128]; long views; int comments; };
    struct poststat posts[512];
    int np = 0;
    long post_views = 0;
    char *idx = db_get("posts_index");
    if (idx) {
        char *cp = strdup(idx);
        for (char *t = strtok(cp, ","); t && np < 512; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!*t) continue;
            char key[80]; snprintf(key, sizeof(key), "post_%s", t);
            char *pj = db_get(key);
            if (!pj) continue;
            strncpy(posts[np].id, t, sizeof(posts[np].id) - 1);
            json_field(pj, "title", posts[np].title, sizeof(posts[np].title));
            json_field(pj, "category", posts[np].cat, sizeof(posts[np].cat));
            free(pj);
            char vk[80]; snprintf(vk, sizeof(vk), "views_%s", t);
            posts[np].views = db_get_long(vk, 0);
            post_views += posts[np].views;
            char ck[80]; snprintf(ck, sizeof(ck), "comments_%s", t);
            char *c = db_get(ck);
            posts[np].comments = 0;
            if (c) {
                for (char *q = c; *q; q++)
                    if (!strncmp(q, "\"author\"", 8)) posts[np].comments++;
                free(c);
            }
            np++;
        }
        free(cp); free(idx);
    }
    /* sort desc by views */
    for (int i = 0; i < np; i++)
        for (int j = i + 1; j < np; j++)
            if (posts[j].views > posts[i].views) {
                struct poststat tmp = posts[i]; posts[i] = posts[j]; posts[j] = tmp;
            }

    /* ---- countries ---- */
    struct ctrstat { char cc[3], name[64]; long views; };
    struct ctrstat ctrs[256];
    int nc = 0;
    char *ci = db_get("ctr_index");
    if (ci) {
        char *cp = strdup(ci);
        for (char *t = strtok(cp, ","); t && nc < 256; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!*t) continue;
            char ck[16]; snprintf(ck, sizeof(ck), "ctr_%s", t);
            long v = db_get_long(ck, 0);
            if (!v) continue;
            strncpy(ctrs[nc].cc, t, 2); ctrs[nc].cc[2] = 0;
            char nk[24]; snprintf(nk, sizeof(nk), "ctrname_%s", t);
            char *nm = db_get(nk);
            strncpy(ctrs[nc].name, nm ? nm : t, sizeof(ctrs[nc].name) - 1);
            if (nm) free(nm);
            ctrs[nc].views = v;
            nc++;
        }
        free(cp); free(ci);
    }
    for (int i = 0; i < nc; i++)
        for (int j = i + 1; j < nc; j++)
            if (ctrs[j].views > ctrs[i].views) {
                struct ctrstat tmp = ctrs[i]; ctrs[i] = ctrs[j]; ctrs[j] = tmp;
            }

    /* ---- daily (last 14, chronological) ---- */
    struct { char date[16]; long views; } days[90];
    int nd = 0;
    char *di = db_get("daily_index");
    if (di) {
        char *cp = strdup(di);
        for (char *t = strtok(cp, ","); t && nd < 90; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!*t) continue;
            char dk[32]; snprintf(dk, sizeof(dk), "daily_%s", t);
            strncpy(days[nd].date, t, sizeof(days[nd].date) - 1);
            days[nd].views = db_get_long(dk, 0);
            nd++;
        }
        free(cp); free(di);
    }
    int dstart = nd > 14 ? nd - 14 : 0;

    char *recent = db_get("viewlog");
    if (!recent) recent = strdup("[]");
    size_t evts = fdb_tsl_query_count(&g_tsdb, 0, (fdb_time_t)time(NULL) + 86400, FDB_TSL_WRITE);

    sb_add(&b, "{\"total_views\":%ld,\"today_views\":%ld,\"post_views\":%ld,\"events\":%zu,",
           total, today_v, post_views, evts);
    /* top post */
    if (np > 0) {
        char et[1024]; json_escape_into(posts[0].title, et, sizeof(et));
        sb_add(&b, "\"top_post\":{\"id\":\"%s\",\"title\":\"%s\",\"views\":%ld},",
               posts[0].id, et, posts[0].views);
    } else sb_add(&b, "\"top_post\":null,");
    /* per post */
    sb_add(&b, "\"per_post\":[");
    for (int i = 0; i < np; i++) {
        char et[1024], ec[256]; 
        json_escape_into(posts[i].title, et, sizeof(et));
        json_escape_into(posts[i].cat, ec, sizeof(ec));
        sb_add(&b, "%s{\"id\":\"%s\",\"title\":\"%s\",\"category\":\"%s\",\"views\":%ld,\"comments\":%d}",
               i ? "," : "", posts[i].id, et, ec, posts[i].views, posts[i].comments);
    }
    sb_add(&b, "],\"countries\":[");
    for (int i = 0; i < nc; i++) {
        char en[128]; json_escape_into(ctrs[i].name, en, sizeof(en));
        double pct = total ? (100.0 * ctrs[i].views / total) : 0;
        sb_add(&b, "%s{\"cc\":\"%s\",\"name\":\"%s\",\"views\":%ld,\"pct\":%.1f}",
               i ? "," : "", ctrs[i].cc, en, ctrs[i].views, pct);
    }
    sb_add(&b, "],\"daily\":[");
    for (int i = dstart; i < nd; i++)
        sb_add(&b, "%s{\"date\":\"%s\",\"views\":%ld}", i > dstart ? "," : "", days[i].date, days[i].views);
    sb_add(&b, "],\"recent\":%s}", recent);
    free(recent);
    return b.s;
}

/* ---------------- HTTP ---------------- */
static void send_all(int fd, const char *buf, size_t n) {
    size_t s = 0;
    while (s < n) {
        ssize_t w = send(fd, buf + s, n - s, 0);
        if (w <= 0) break;
        s += (size_t) w;
    }
}

static void http_reply(int fd, int code, const char *status, const char *ctype,
                       const char *extra_hdr, const char *body, size_t blen) {
    char h[MAX_RESP_HEAD];
    int hl = snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n%s%s\r\n",
        code, status, ctype, blen, extra_hdr ? extra_hdr : "", extra_hdr ? "" : "");
    (void)hl;
    send_all(fd, h, strlen(h));
    if (blen) send_all(fd, body, blen);
}

static void reply_json(int fd, const char *json) {
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", "Cache-Control: no-store\r\n", json, strlen(json));
}
static void reply_err(int fd, int code, const char *msg) {
    char b[512]; snprintf(b, sizeof(b), "{\"error\":\"%s\"}", msg);
    const char *st = code == 404 ? "Not Found" : code == 401 ? "Unauthorized" :
                     code == 403 ? "Forbidden" : code == 400 ? "Bad Request" : "Error";
    http_reply(fd, code, st, "application/json; charset=utf-8", NULL, b, strlen(b));
}

static const char *get_header(const char *hdrs, const char *name) {
    static char val[4096];
    size_t nl = strlen(name);
    const char *p = hdrs;
    while (*p) {
        const char *e = strstr(p, "\r\n");
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll > nl + 1 && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= sizeof(val)) n = sizeof(val) - 1;
            memcpy(val, v, n); val[n] = 0;
            return val;
        }
        if (!e) break;
        p = e + 2;
    }
    return NULL;
}

static int serve_static(int fd, const char *urlpath) {
    char rel[1024];
    if (strcmp(urlpath, "/") == 0) strcpy(rel, "/index.html");
    else if (strcmp(urlpath, "/post") == 0 || strncmp(urlpath, "/post/", 6) == 0) strcpy(rel, "/post.html");
    else if (strcmp(urlpath, "/about") == 0) strcpy(rel, "/about.html");
    else if (strcmp(urlpath, "/admin") == 0 || strcmp(urlpath, "/login") == 0) strcpy(rel, "/admin.html");
    else { strncpy(rel, urlpath, sizeof(rel)-1); rel[sizeof(rel)-1] = 0; }
    if (strstr(rel, "..")) { reply_err(fd, 403, "forbidden"); return 1; }
    char *q = strchr(rel, '?'); if (q) *q = 0;
    if (rel[strlen(rel)-1] == '/') strncat(rel, "index.html", sizeof(rel)-strlen(rel)-1);
    char full[1536]; snprintf(full, sizeof(full), "%s%s", g_public_dir, rel);
    FILE *f = fopen(full, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 8*1024*1024) { fclose(f); return 0; }
    char *data = malloc((size_t)sz ? (size_t)sz : 1);
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(data); return 0; }
    fclose(f);
    http_reply(fd, 200, "OK", mime_for(full), "Cache-Control: no-cache\r\n", data, (size_t)sz);
    free(data);
    return 1;
}

/* ---------- API handlers ---------- */
static void handle_api(int fd, const char *method, const char *path,
                       const char *qs, const char *cookie, const char *body,
                       const char *client_ip) {
    /* login */
    if (!strcmp(path, "/api/login") && !strcmp(method, "POST")) {
        char u[128] = {0}, p[128] = {0};
        json_field(body, "username", u, sizeof(u));
        json_field(body, "password", p, sizeof(p));
        if (!u[0]) query_param(body, "username", u, sizeof(u));
        if (!p[0]) query_param(body, "password", p, sizeof(p));
        /* allow urlencoded body */
        if (!u[0] || !p[0]) {
            char tmp[1024]; strncpy(tmp, body, sizeof(tmp)-1);
            query_param(tmp, "username", u, sizeof(u));
            query_param(tmp, "password", p, sizeof(p));
            url_decode(u); url_decode(p);
        }
        if (!strcmp(u, ADMIN_USER) && !strcmp(p, ADMIN_PASS)) {
            char tok[33]; make_token(tok, sizeof(tok));
            pthread_mutex_lock(&g_token_locker);
            strcpy(g_admin_token, tok);
            pthread_mutex_unlock(&g_token_locker);
            db_set("admin_session", tok);
            char extra[256];
            snprintf(extra, sizeof(extra),
                "Set-Cookie: session=%s; Path=/; HttpOnly; SameSite=Lax\r\n", tok);
            const char *jb = "{\"ok\":true,\"user\":\"kono\"}";
            http_reply(fd, 200, "OK", "application/json; charset=utf-8", extra, jb, strlen(jb));
            tsdb_log("login:ok");
        } else {
            tsdb_log("login:fail");
            reply_err(fd, 401, "invalid credentials (hint kono/kono)");
        }
        return;
    }
    if (!strcmp(path, "/api/logout") && !strcmp(method, "POST")) {
        pthread_mutex_lock(&g_token_locker); g_admin_token[0] = 0; pthread_mutex_unlock(&g_token_locker);
        db_del("admin_session");
        http_reply(fd, 200, "OK", "application/json; charset=utf-8",
            "Set-Cookie: session=; Path=/; Max-Age=0\r\n", "{\"ok\":true}", 11);
        return;
    }
    if (!strcmp(path, "/api/me") && !strcmp(method, "GET")) {
        char b[160]; snprintf(b, sizeof(b), "{\"admin\":%s}", is_admin(cookie) ? "true" : "false");
        reply_json(fd, b); return;
    }
    /* public list */
    if ((!strcmp(path, "/api/posts")) && !strcmp(method, "GET")) {
        char search[256] = {0}, cat[128] = {0}, pg[16] = {0}, pp[16] = {0};
        query_param(qs, "search", search, sizeof(search));
        query_param(qs, "q", search, sizeof(search));
        query_param(qs, "category", cat, sizeof(cat));
        query_param(qs, "page", pg, sizeof(pg));
        query_param(qs, "per", pp, sizeof(pp));
        char *j = api_list_posts(search, cat, atoi(pg), atoi(pp));
        reply_json(fd, j); free(j); return;
    }
    if (!strcmp(path, "/api/search") && !strcmp(method, "GET")) {
        char q[256] = {0}; query_param(qs, "q", q, sizeof(q));
        char *j = api_list_posts(q, "", 1, 12);
        reply_json(fd, j); free(j); return;
    }
    if (!strcmp(path, "/api/post") && !strcmp(method, "GET")) {
        char id[64] = {0}, slug[192] = {0}, ccraw[16] = {0}, craw[64] = {0}, vraw[8] = {0};
        query_param(qs, "id", id, sizeof(id));
        query_param(qs, "slug", slug, sizeof(slug));
        query_param(qs, "cc", ccraw, sizeof(ccraw));
        query_param(qs, "country", craw, sizeof(craw));
        query_param(qs, "view", vraw, sizeof(vraw));
        if (!id[0] && slug[0]) { /* slug lookup -> numeric id */
            char rid[64];
            if (!post_id_by_slug(slug, rid, sizeof(rid))) { reply_err(fd, 404, "post not found"); return; }
            strcpy(id, rid);
        }
        if (!id[0]) { reply_err(fd, 400, "missing id"); return; }
        char cc[3], cname[64];
        sanitize_cc(ccraw, cc);
        sanitize_country(craw, cname, sizeof(cname));
        if (!strcmp(cc, "XX")) strcpy(cname, "Unknown");
        int inc = strcmp(vraw, "0") ? 1 : 0; /* ?view=0 reads without counting */
        char *j = api_get_post(id, inc, client_ip, cc, cname);
        if (!j) { reply_err(fd, 404, "post not found"); return; }
        reply_json(fd, j); free(j); return;
    }
    /* lightweight page-view beacon (home / about / etc). Public, no post counter touched. */
    if (!strcmp(path, "/api/track") && !strcmp(method, "POST")) {
        char page[64] = {0}, ccraw[16] = {0}, craw[64] = {0};
        json_field(body, "page", page, sizeof(page));
        json_field(body, "cc", ccraw, sizeof(ccraw));
        json_field(body, "country", craw, sizeof(craw));
        if (!page[0]) query_param(qs, "page", page, sizeof(page));
        /* validate slug */
        int ok = page[0] != 0;
        for (char *p = page; *p; p++)
            if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') ok = 0;
        if (!ok) { reply_err(fd, 400, "bad page"); return; }
        char cc[3], cname[64];
        sanitize_cc(ccraw, cc);
        sanitize_country(craw, cname, sizeof(cname));
        if (!strcmp(cc, "XX")) strcpy(cname, "Unknown");
        track_view(page, 0, client_ip, cc, cname);
        reply_json(fd, "{\"ok\":true}");
        return;
    }
    if (!strcmp(path, "/api/comments") && !strcmp(method, "GET")) {
        char pid[64] = {0}; query_param(qs, "post_id", pid, sizeof(pid));
        if (!pid[0]) query_param(qs, "id", pid, sizeof(pid));
        if (!pid[0]) { reply_err(fd, 400, "missing post_id"); return; }
        char ck[80]; snprintf(ck, sizeof(ck), "comments_%s", pid);
        char *c = db_get(ck); if (!c) c = strdup("[]");
        char *out = malloc(strlen(c) + 64);
        sprintf(out, "{\"comments\":%s}", c);
        reply_json(fd, out); free(out); free(c); return;
    }
    if (!strcmp(path, "/api/comments") && !strcmp(method, "POST")) {
        char pid[64] = {0}, author[128] = {0}, text[4096] = {0};
        json_field(body, "post_id", pid, sizeof(pid));
        json_field(body, "author", author, sizeof(author));
        json_field(body, "text", text, sizeof(text));
        if (!pid[0]) query_param(qs, "post_id", pid, sizeof(pid));
        if (!pid[0] || !author[0] || !text[0]) { reply_err(fd, 400, "post_id, author, text required"); return; }
        if (strlen(author) > 60 || strlen(text) > 2000) { reply_err(fd, 400, "too long"); return; }
        /* verify post exists */
        char pk[80]; snprintf(pk, sizeof(pk), "post_%s", pid);
        char *chk = db_get(pk); if (!chk) { reply_err(fd, 404, "post not found"); return; } free(chk);
        char ck[80]; snprintf(ck, sizeof(ck), "comments_%s", pid);
        char *cur = db_get(ck);
        char date[16]; today_iso(date, sizeof(date));
        char ea[256], et[8192]; json_escape_into(author, ea, sizeof(ea)); json_escape_into(text, et, sizeof(et));
        char item[9000]; snprintf(item, sizeof(item), "{\"author\":\"%s\",\"text\":\"%s\",\"date\":\"%s\"}", ea, et, date);
        char *next;
        if (!cur || !strcmp(cur, "[]") || !cur[0]) next = malloc(strlen(item)+8), sprintf(next, "[%s]", item);
        else { cur[strlen(cur)-1] = 0; next = malloc(strlen(cur)+strlen(item)+8); sprintf(next, "%s,%s]", cur, item); }
        free(cur);
        db_set(ck, next); free(next);
        char ev[160]; snprintf(ev, sizeof(ev), "comment:%s:%ld", pid, (long)time(NULL));
        tsdb_log(ev);
        reply_json(fd, "{\"ok\":true}");
        return;
    }
    if (!strcmp(path, "/api/newsletter") && !strcmp(method, "POST")) {
        char email[256] = {0};
        json_field(body, "email", email, sizeof(email));
        if (!email[0]) { char t[512]; strncpy(t, body, sizeof(t)-1); query_param(t, "email", email, sizeof(email)); url_decode(email); }
        if (!strchr(email, '@') || strlen(email) < 5 || strlen(email) > 120) { reply_err(fd, 400, "invalid email"); return; }
        char *cur = db_get("newsletter_list"); if (!cur) cur = strdup("");
        if (strstr(cur, email)) { free(cur); reply_json(fd, "{\"ok\":true,\"dup\":true}"); return; }
        char *next = malloc(strlen(cur) + strlen(email) + 4);
        sprintf(next, "%s%s%s", cur, *cur ? "\n" : "", email);
        db_set("newsletter_list", next);
        free(cur); free(next);
        tsdb_log("newsletter:join");
        reply_json(fd, "{\"ok\":true}");
        return;
    }
    /* ---- admin only below ---- */
    int admin = is_admin(cookie);
    if (!strcmp(path, "/api/analytics") && !strcmp(method, "GET")) {
        if (!admin) { reply_err(fd, 401, "admin only"); return; }
        char *j = api_analytics();
        reply_json(fd, j); free(j); return;
    }
    if (!strcmp(path, "/api/stats") && !strcmp(method, "GET")) {
        if (!admin) { reply_err(fd, 401, "admin only"); return; }
        char *idx = db_get("posts_index"); if (!idx) idx = strdup("");
        int posts = 0; if (*idx) { posts = 1; for (char *p = idx; *p; p++) if (*p == ',') posts++; }
        long sv = db_get_long("site_views", 0);
        char *nl = db_get("newsletter_list");
        int subs = 0;
        if (nl && *nl) { subs = 1; for (char *p = nl; *p; p++) if (*p == '\n') subs++; }
        int comments = 0;
        /* count comments by scanning index */
        char *cp = strdup(idx);
        for (char *t = strtok(cp, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!*t) continue;
            char ck[80]; snprintf(ck, sizeof(ck), "comments_%s", t);
            char *c = db_get(ck);
            if (c) { for (char *q2 = c; *q2; q2++) if (!strncmp(q2, "\"author\"", 8)) comments++; free(c); }
        }
        free(cp);
        size_t tsl_total = 0;
        { /* tsdb count via iterate not trivial; use site_views + comments as proxy + query */
          tsl_total = fdb_tsl_query_count(&g_tsdb, 0, (fdb_time_t)time(NULL)+86400, FDB_TSL_WRITE); }
        char b[1024];
        snprintf(b, sizeof(b),
            "{\"posts\":%d,\"views\":%ld,\"subscribers\":%d,\"comments\":%d,\"events\":%zu}",
            posts, sv, subs, comments, tsl_total);
        free(idx); if (nl) free(nl);
        reply_json(fd, b); return;
    }
    if (!strcmp(path, "/api/subscribers") && !strcmp(method, "GET")) {
        if (!admin) { reply_err(fd, 401, "admin only"); return; }
        char *nl = db_get("newsletter_list"); if (!nl) nl = strdup("");
        /* to JSON array */
        char *out = malloc(strlen(nl) + 64);
        char *o = out; o += sprintf(o, "{\"emails\":[");
        int first = 1;
        for (char *line = strtok(nl, "\n"); line; line = strtok(NULL, "\n")) {
            char e[512]; json_escape_into(line, e, sizeof(e));
            o += sprintf(o, "%s\"%s\"", first ? "" : ",", e); first = 0;
        }
        sprintf(o, "]}");
        reply_json(fd, out); free(out); free(nl); return;
    }
    if (!strcmp(path, "/api/posts") && !strcmp(method, "POST")) {
        if (!admin) { reply_err(fd, 401, "admin only"); return; }
        char title[512] = {0}, cat[128] = {0}, excerpt[2048] = {0}, image[1024] = {0},
             content[20000] = {0}, tags[512] = {0}, author[128] = {0}, video[32] = {0};
        json_field(body, "title", title, sizeof(title));
        json_field(body, "category", cat, sizeof(cat));
        json_field(body, "excerpt", excerpt, sizeof(excerpt));
        json_field(body, "image", image, sizeof(image));
        json_field(body, "content", content, sizeof(content));
        json_field(body, "tags", tags, sizeof(tags));
        json_field(body, "author", author, sizeof(author));
        json_field(body, "video", video, sizeof(video));
        int featured = json_field_int(body, "featured", 0);
        if (!title[0] || !content[0]) { reply_err(fd, 400, "title and content required"); return; }
        if (!valid_video(video)) { reply_err(fd, 400, "bad video id"); return; }
        if (!cat[0]) strcpy(cat, "News");
        if (!author[0]) strcpy(author, "Kono");
        if (!image[0]) strcpy(image, "https://images.unsplash.com/photo-1493514789931-586cb221d7a7?w=1200&q=80");
        if (!excerpt[0]) snprintf(excerpt, sizeof(excerpt), "%.160s...", content);
        long seq = db_get_long("seq_post", 7);
        char id[32]; snprintf(id, sizeof(id), "%ld", seq);
        seed_post(id, title, cat, excerpt, image, tags, author, featured, video, content);
        /* Verify the post was actually saved */
        char verify_key[64]; snprintf(verify_key, sizeof(verify_key), "post_%s", id);
        char *verify = db_get(verify_key);
        if (!verify) {
            reply_err(fd, 500, "failed to save post (content may be too large)");
            return;
        }
        free(verify);
        /* Post saved OK — now update index and counter */
        db_set("seq_post", (char[]) {0}); /* keep helper warm */
        char ns[32]; snprintf(ns, sizeof(ns), "%ld", seq + 1);
        db_set("seq_post", ns);
        char *idx = db_get("posts_index"); if (!idx) idx = strdup("");
        char *nidx = malloc(strlen(idx) + 32);
        sprintf(nidx, "%s%s%s", id, *idx ? "," : "", idx);
        db_set("posts_index", nidx);
        free(idx); free(nidx);
        tsdb_log("post:create");
        char b[384], bslug[192] = {0};
        { char *vj = db_get(verify_key); if (vj) { json_field(vj, "slug", bslug, sizeof(bslug)); free(vj); } }
        snprintf(b, sizeof(b), "{\"ok\":true,\"id\":\"%s\",\"slug\":\"%s\"}", id, bslug);
        reply_json(fd, b); return;
    }
    if (!strcmp(path, "/api/posts") && (!strcmp(method, "PUT") || !strcmp(method, "PATCH"))) {
        if (!admin) { reply_err(fd, 401, "admin only"); return; }
        char id[64] = {0}; query_param(qs, "id", id, sizeof(id));
        if (!id[0]) json_field(body, "id", id, sizeof(id));
        if (!id[0]) { reply_err(fd, 400, "missing id"); return; }
        char key[80]; snprintf(key, sizeof(key), "post_%s", id);
        char *old = db_get(key); if (!old) { reply_err(fd, 404, "not found"); return; }
        char title[512] = {0}, cat[128] = {0}, excerpt[2048] = {0}, image[1024] = {0},
             content[20000] = {0}, tags[512] = {0}, author[128] = {0}, date[32] = {0}, video[32] = {0};
        json_field(old, "title", title, sizeof(title));
        json_field(old, "category", cat, sizeof(cat));
        json_field(old, "excerpt", excerpt, sizeof(excerpt));
        json_field(old, "image", image, sizeof(image));
        json_field(old, "content", content, sizeof(content));
        json_field(old, "tags", tags, sizeof(tags));
        json_field(old, "author", author, sizeof(author));
        json_field(old, "date", date, sizeof(date));
        json_field(old, "video", video, sizeof(video));
        char slug[192] = {0};
        json_field(old, "slug", slug, sizeof(slug));
        int featured = json_field_int(old, "featured", 0);
        free(old);
        char t2[512]; if (json_field(body, "title", t2, sizeof(t2))) strcpy(title, t2);
        char c2[128]; if (json_field(body, "category", c2, sizeof(c2))) strcpy(cat, c2);
        char e2[2048]; if (json_field(body, "excerpt", e2, sizeof(e2))) strcpy(excerpt, e2);
        char i2[1024]; if (json_field(body, "image", i2, sizeof(i2))) strcpy(image, i2);
        char x2[20000]; if (json_field(body, "content", x2, sizeof(x2))) strcpy(content, x2);
        char g2[512]; if (json_field(body, "tags", g2, sizeof(g2))) strcpy(tags, g2);
        char a2[128]; if (json_field(body, "author", a2, sizeof(a2))) strcpy(author, a2);
        char v2[32]; if (json_field(body, "video", v2, sizeof(v2))) {
            if (!valid_video(v2)) { reply_err(fd, 400, "bad video id"); return; }
            strcpy(video, v2);
        }
        char f2[16]; if (json_field(body, "featured", f2, sizeof(f2))) featured = atoi(f2);
        /* rebuild preserving date + slug (slugs stay stable so links don't rot) */
        char *json = malloc(MAX_POST_SIZE);
        char et[4096], ee[8192], ec[16000], eg[1024], ei[1024], ea[256], ce[256], ev[32], es[256];
        json_escape_into(title, et, sizeof(et)); json_escape_into(excerpt, ee, sizeof(ee));
        json_escape_into(content, ec, sizeof(ec)); json_escape_into(tags, eg, sizeof(eg));
        json_escape_into(image, ei, sizeof(ei)); json_escape_into(author, ea, sizeof(ea));
        json_escape_into(cat, ce, sizeof(ce)); json_escape_into(video, ev, sizeof(ev));
        if (!slug[0]) { /* edited post from pre-slug era: mint one now */
            char base[96]; make_slug(title[0] ? title : id, id, base, sizeof(base));
            slug_unique(base, id, slug, sizeof(slug));
            char sk[256]; snprintf(sk, sizeof(sk), "slug_%s", slug);
            db_set(sk, id);
        }
        json_escape_into(slug, es, sizeof(es));
        if (!date[0]) today_iso(date, sizeof(date));
        snprintf(json, MAX_POST_SIZE,
            "{\"id\":\"%s\",\"slug\":\"%s\",\"title\":\"%s\",\"category\":\"%s\",\"excerpt\":\"%s\","
            "\"image\":\"%s\",\"tags\":\"%s\",\"author\":\"%s\",\"date\":\"%s\",\"featured\":%d,"
            "\"video\":\"%s\",\"content\":\"%s\"}", id, es, et, ce, ee, ei, eg, ea, date, featured, ev, ec);
        db_set(key, json); free(json);
        tsdb_log("post:update");
        reply_json(fd, "{\"ok\":true}"); return;
    }
    if (!strcmp(path, "/api/posts") && !strcmp(method, "DELETE")) {
        if (!admin) { reply_err(fd, 401, "admin only"); return; }
        char id[64] = {0}; query_param(qs, "id", id, sizeof(id));
        if (!id[0]) { reply_err(fd, 400, "missing id"); return; }
        char key[80]; snprintf(key, sizeof(key), "post_%s", id);
        char *gone = db_get(key);
        if (gone) {
            char sl[192] = {0};
            json_field(gone, "slug", sl, sizeof(sl));
            if (sl[0]) { char sk[256]; snprintf(sk, sizeof(sk), "slug_%s", sl); db_del(sk); }
            free(gone);
        }
        db_del(key);
        char vk[80]; snprintf(vk, sizeof(vk), "views_%s", id); db_del(vk);
        char ck[80]; snprintf(ck, sizeof(ck), "comments_%s", id); db_del(ck);
        char *idx = db_get("posts_index"); if (!idx) idx = strdup("");
        char nidx[4096] = {0};
        char *cp = strdup(idx);
        int first = 1;
        for (char *t = strtok(cp, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!*t || !strcmp(t, id)) continue;
            strcat(nidx, first ? "" : ","); strcat(nidx, t); first = 0;
        }
        free(cp); free(idx);
        db_set("posts_index", nidx);
        tsdb_log("post:delete");
        reply_json(fd, "{\"ok\":true}"); return;
    }
    if (!strcmp(path, "/api/comment") && !strcmp(method, "DELETE")) {
        if (!admin) { reply_err(fd, 401, "admin only"); return; }
        char pid[64] = {0}, sx[16] = {0};
        query_param(qs, "post_id", pid, sizeof(pid));
        query_param(qs, "index", sx, sizeof(sx));
        if (!pid[0] || !sx[0]) { reply_err(fd, 400, "post_id + index required"); return; }
        int del = atoi(sx);
        char ck[80]; snprintf(ck, sizeof(ck), "comments_%s", pid);
        char *cur = db_get(ck); if (!cur) { reply_err(fd, 404, "no comments"); return; }
        /* naive: split top-level {...} items */
        char *items[512]; int nc = 0;
        for (char *p = cur; *p && nc < 512; ) {
            while (*p && *p != '{') p++;
            if (!*p) break;
            int depth = 0, instr = 0;
            char *st = p;
            while (*p) {
                if (*p == '"' && p[-1] != '\\') instr = !instr;
                if (!instr) { if (*p == '{') depth++; if (*p == '}') { depth--; if (!depth) { p++; break; } } }
                p++;
            }
            size_t L = (size_t)(p - st);
            items[nc] = malloc(L + 1); memcpy(items[nc], st, L); items[nc][L] = 0; nc++;
            while (*p && (*p == ',' || *p == ']' || isspace((unsigned char)*p))) { if (*p == ']') break; p++; }
            if (*p == ']') break;
        }
        free(cur);
        if (del < 0 || del >= nc) { for (int i = 0; i < nc; i++) free(items[i]); reply_err(fd, 400, "bad index"); return; }
        free(items[del]);
        for (int i = del; i + 1 < nc; i++) items[i] = items[i+1];
        nc--;
        size_t cap = 16; for (int i = 0; i < nc; i++) cap += strlen(items[i]) + 2;
        char *next = malloc(cap);
        char *o = next; o += sprintf(o, "[");
        for (int i = 0; i < nc; i++) { o += sprintf(o, "%s%s", i ? "," : "", items[i]); free(items[i]); }
        sprintf(o, "]");
        db_set(ck, next); free(next);
        reply_json(fd, "{\"ok\":true}"); return;
    }
    reply_err(fd, 404, "unknown api");
}

/* ---------- connection ---------- */
struct client_arg { int fd; char ip[64]; };

static void *client_thread(void *arg) {
    struct client_arg *ca = arg;
    int fd = ca->fd;
    char ip[64]; strcpy(ip, ca->ip); free(ca);
    pthread_detach(pthread_self());

    char *req = malloc(MAX_REQ + 1);
    if (!req) { close(fd); return NULL; }
    ssize_t total = 0;
    ssize_t r = recv(fd, req, MAX_REQ, 0);
    if (r <= 0) { free(req); close(fd); return NULL; }
    total = r;
    req[total] = 0;
    char *hend = strstr(req, "\r\n\r\n");
    int clen = 0;
    if (hend) {
        char *tmp = strndup(req, (size_t)(hend - req));
        const char *cl = get_header(tmp, "Content-Length");
        /* get_header uses static buf; copy immediately */
        char clb[32] = {0};
        if (cl) strncpy(clb, cl, sizeof(clb)-1);
        if (clb[0]) clen = atoi(clb);
        free(tmp);
        size_t hlen = (size_t)(hend - req) + 4;
        while ((size_t)total < hlen + (size_t)clen && (size_t)total < MAX_REQ) {
            r = recv(fd, req + total, MAX_REQ - total, 0);
            if (r <= 0) break;
            total += r;
        }
        req[total] = 0;
    }

    char method[16] = {0}, rawpath[2048] = {0};
    sscanf(req, "%15s %2047s", method, rawpath);
    char *qs = strchr(rawpath, '?');
    char qbuf[2048] = {0};
    if (qs) { strncpy(qbuf, qs + 1, sizeof(qbuf)-1); *qs = 0; }
    char path[2048]; strncpy(path, rawpath, sizeof(path)-1); path[sizeof(path)-1] = 0;

    char *hdrs = strchr(req, '\n');
    const char *cookie = NULL;
    char cookie_buf[4096] = {0};
    char ip_eff[64];
    strncpy(ip_eff, ip, sizeof(ip_eff) - 1); ip_eff[sizeof(ip_eff)-1] = 0;
    if (hdrs) {
        const char *c = get_header(hdrs, "Cookie");
        if (c) { strncpy(cookie_buf, c, sizeof(cookie_buf)-1); cookie = cookie_buf; }
        /* reverse-proxy aware: trust X-Forwarded-For's leftmost (client) IP */
        const char *xff = get_header(hdrs, "X-Forwarded-For");
        if (xff) {
            char tmp[128]; strncpy(tmp, xff, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = 0;
            char *comma = strchr(tmp, ','); if (comma) *comma = 0;
            while (*tmp == ' ' || *tmp == '\t') memmove(tmp, tmp+1, strlen(tmp));
            size_t L = strlen(tmp);
            while (L && (tmp[L-1] == ' ' || tmp[L-1] == '\t')) tmp[--L] = 0;
            int ok = L > 0 && L < sizeof(ip_eff);
            for (size_t i = 0; ok && i < L; i++)
                if (!isxdigit((unsigned char)tmp[i]) && tmp[i] != '.' && tmp[i] != ':') ok = 0;
            if (ok) strcpy(ip_eff, tmp);
        }
    }
    char *body = hend ? hend + 4 : req + total;
    static char empty[] = "";
    if (!body) body = empty;

    if (!strcmp(method, "OPTIONS")) {
        http_reply(fd, 204, "No Content", "text/plain", "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Cookie\r\n", "", 0);
    } else if (!strncmp(path, "/api/", 5)) {
        handle_api(fd, method, path, qbuf, cookie, body, ip_eff);
    } else {
        if (strcmp(method, "GET") && strcmp(method, "HEAD")) reply_err(fd, 405, "method not allowed");
        else if (!serve_static(fd, path)) {
            const char *nf = "<h1>404 - Lost in Leonida</h1><p><a href='/'>Back to Vice City</a></p>";
            http_reply(fd, 404, "Not Found", "text/html", NULL, nf, strlen(nf));
        }
    }
    free(req);
    close(fd);
    return NULL;
}

/* ---------------- sector-size migration ----------------
 * FlashDB KVDB enforces that each KV must fit in one sector.
 * Old config: sec_size=4096 (max ~4KB per value).
 * New config: sec_size=65536 (max ~64KB per value).
 * On first boot with new config, export all keys, delete old files, reinit. */
#define OLD_SEC_SIZE  4096
#define OLD_MAX_SIZE  (OLD_SEC_SIZE * 64)
#define NEW_SEC_SIZE  65536
#define NEW_MAX_SIZE  (10 * 1024 * 1024)

/* Helper: initialize KVDB with given sector/max size, return FDB_NO_ERR on success */
static fdb_err_t try_init_kvdb(struct fdb_kvdb *db, uint32_t sec_sz, uint32_t max_sz) {
    bool fm = true;
    fdb_kvdb_control(db, FDB_KVDB_CTRL_SET_LOCK, kv_lock);
    fdb_kvdb_control(db, FDB_KVDB_CTRL_SET_UNLOCK, kv_unlock);
    fdb_kvdb_control(db, FDB_KVDB_CTRL_SET_SEC_SIZE, &sec_sz);
    fdb_kvdb_control(db, FDB_KVDB_CTRL_SET_MAX_SIZE, &max_sz);
    fdb_kvdb_control(db, FDB_KVDB_CTRL_SET_FILE_MODE, &fm);
    struct fdb_default_kv dv = { NULL, 0 };
    return fdb_kvdb_init(db, "blog", "data/fdb_kvdb", &dv, NULL);
}

static int needs_migration(void) {
    /* If old-size sector files exist but new-size ones don't, we need migration.
     * Old files: blog.fdb.0 .. blog.fdb.63 (each 4096 bytes) */
    struct stat st;
    if (stat("data/fdb_kvdb/blog.fdb.0", &st) != 0) return 0; /* no old files */
    /* If file size matches old config (4096), migration needed */
    if (st.st_size == OLD_SEC_SIZE) {
        /* Check if we already migrated (marker key exists) */
        return 1;
    }
    return 0;
}

static void migrate_sector_size(void) {
    fprintf(stderr, "[migrate] Old sector size detected, migrating...\n");

    /* We can't read old DB with new sector size.
     * Strategy: delete old files, reinit new, then re-seed.
     * Posts will be re-created by ensure_seed() for IDs 1-4.
     * For posts 5+, we need to reimport via API after startup. */
    char cmd[256];

    /* Phase 1: Delete old sector files */
    snprintf(cmd, sizeof(cmd), "rm -f data/fdb_kvdb/blog.fdb.*");
    system(cmd);
    fprintf(stderr, "[migrate] Deleted old KVDB sector files\n");

    /* Phase 2: Init with NEW sector size (fresh DB) */
    if (try_init_kvdb(&g_kvdb, NEW_SEC_SIZE, NEW_MAX_SIZE) != FDB_NO_ERR) {
        fprintf(stderr, "[migrate] FATAL: new KVDB init failed\n");
        exit(1);
    }
    fprintf(stderr, "[migrate] New KVDB initialized with %u-byte sectors\n", NEW_SEC_SIZE);

    /* Phase 3: Write migration marker so ensure_seed runs */
    db_set("migrated_v4", "1");
    /* ensure_seed() will be called later and will create posts 1-4 */
    fprintf(stderr, "[migrate] Migration complete. Posts will be re-seeded.\n");
    fprintf(stderr, "[migrate] IMPORTANT: Re-import posts 5+ via API after startup.\n");
}

/* ---------------- boot ---------------- */
static void init_db(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_kv_locker, &attr);
    pthread_mutex_init(&g_ts_locker, &attr);

    mkdir("data", 0777);
    mkdir("data/fdb_kvdb", 0777);
    mkdir("data/fdb_tsdb", 0777);

    /* Check if migration is needed (old sector files exist) */
    if (needs_migration()) {
        migrate_sector_size();
        /* g_kvdb is already initialized by migrate, skip init below */
    } else {
        /* Normal init with new sector size */
        if (try_init_kvdb(&g_kvdb, NEW_SEC_SIZE, NEW_MAX_SIZE) != FDB_NO_ERR) {
            fprintf(stderr, "FATAL: kvdb init failed\n");
            exit(1);
        }
    }

    /* TSDB init with new sector size */
    bool file_mode = true;
    uint32_t ts_sec = NEW_SEC_SIZE, ts_max = NEW_MAX_SIZE;
    fdb_tsdb_control(&g_tsdb, FDB_TSDB_CTRL_SET_LOCK, ts_lock);
    fdb_tsdb_control(&g_tsdb, FDB_TSDB_CTRL_SET_UNLOCK, ts_unlock);
    fdb_tsdb_control(&g_tsdb, FDB_TSDB_CTRL_SET_SEC_SIZE, &ts_sec);
    fdb_tsdb_control(&g_tsdb, FDB_TSDB_CTRL_SET_MAX_SIZE, &ts_max);
    fdb_tsdb_control(&g_tsdb, FDB_TSDB_CTRL_SET_FILE_MODE, &file_mode);
    if (fdb_tsdb_init(&g_tsdb, "views", "data/fdb_tsdb", blog_time, 320, NULL) != FDB_NO_ERR) {
        fprintf(stderr, "FATAL: tsdb init failed\n");
        exit(1);
    }

    /* Restore session */
    char *tok = db_get("admin_session");
    if (tok) {
        pthread_mutex_lock(&g_token_locker);
        strncpy(g_admin_token, tok, sizeof(g_admin_token)-1);
        pthread_mutex_unlock(&g_token_locker);
        free(tok);
    }
    ensure_seed();
    ensure_slugs();
}

int main(int argc, char **argv) {
    const char *penv = getenv("PORT");
    g_port = 8080;
    if (penv && atoi(penv) > 0) g_port = atoi(penv);
    if (argc > 1 && atoi(argv[1]) > 0) g_port = atoi(argv[1]);
    if (argc > 2) g_public_dir = argv[2];
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    init_db();

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t) g_port);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 64) < 0) { perror("listen"); return 1; }

    printf("\n==============================================\n");
    printf("  GTA6 BLOG  - pure C + FlashDB (KVDB + TSDB)\n");
    printf("  http://localhost:%d/        (blog)\n", g_port);
    printf("  http://localhost:%d/admin   (admin kono/kono)\n", g_port);
    printf("  DB: FlashDB file mode @ ./data/\n");
    printf("==============================================\n\n");
    fflush(stdout);

    for (;;) {
        struct sockaddr_in cli; socklen_t cl = sizeof(cli);
        int fd = accept(srv, (struct sockaddr*)&cli, &cl);
        if (fd < 0) { perror("accept"); continue; }
        struct client_arg *ca = malloc(sizeof(*ca));
        ca->fd = fd;
        inet_ntop(AF_INET, &cli.sin_addr, ca->ip, sizeof(ca->ip));
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, ca) != 0) { close(fd); free(ca); }
    }
    return 0;
}
