#define _GNU_SOURCE
#include "ai_usage/core.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *p;
    const char *end;
} jspan_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} strbuf_t;

static void sb_init(strbuf_t *sb, size_t initial_cap) {
    sb->cap = initial_cap ? initial_cap : 4096;
    sb->buf = malloc(sb->cap);
    sb->len = 0;
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_ensure(strbuf_t *sb, size_t extra) {
    if (!sb->buf) return;
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t ncap = sb->cap * 2;
    while (ncap < sb->len + extra + 1) ncap *= 2;
    char *nb = realloc(sb->buf, ncap);
    if (!nb) {
        free(sb->buf);
        sb->buf = NULL;
        return;
    }
    sb->buf = nb;
    sb->cap = ncap;
}

static void sb_append(strbuf_t *sb, const char *s, size_t n) {
    if (!sb->buf || !n) return;
    sb_ensure(sb, n);
    if (!sb->buf) return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(strbuf_t *sb, const char *s) {
    sb_append(sb, s, strlen(s));
}

static void sb_printf(strbuf_t *sb, const char *fmt, ...) {
    if (!sb->buf) return;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(ap2);
        return;
    }
    sb_ensure(sb, (size_t)needed);
    if (sb->buf) {
        vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap2);
        sb->len += (size_t)needed;
    }
    va_end(ap2);
}

static void sb_json_str(strbuf_t *sb, const char *s) {
    sb_puts(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
        unsigned char c = *p;
        if (c == '"') sb_puts(sb, "\\\"");
        else if (c == '\\') sb_puts(sb, "\\\\");
        else if (c == '\b') sb_puts(sb, "\\b");
        else if (c == '\f') sb_puts(sb, "\\f");
        else if (c == '\n') sb_puts(sb, "\\n");
        else if (c == '\r') sb_puts(sb, "\\r");
        else if (c == '\t') sb_puts(sb, "\\t");
        else if (c < 0x20 || c >= 0x7f) sb_printf(sb, "\\u%04x", (unsigned int)c);
        else sb_append(sb, (const char *)p, 1);
    }
    sb_puts(sb, "\"");
}

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

static const char *skip_string(const char *p, const char *end) {
    if (p >= end || *p != '"') return end;
    ++p;
    while (p < end) {
        if (*p == '\\') {
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        ++p;
    }
    return end;
}

static const char *skip_value(const char *p, const char *end) {
    p = skip_ws(p, end);
    if (p >= end) return end;
    if (*p == '"') return skip_string(p, end);
    if (*p == '{' || *p == '[') {
        char open_c = *p;
        char close_c = (open_c == '{') ? '}' : ']';
        int depth = 1;
        ++p;
        while (p < end && depth > 0) {
            if (*p == '"') {
                p = skip_string(p, end);
            } else {
                if (*p == open_c) ++depth;
                else if (*p == close_c) --depth;
                ++p;
            }
        }
        return p;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        ++p;
    }
    return p;
}

static bool jspan_key_eq(jspan_t raw_str, const char *key) {
    if (raw_str.end - raw_str.p < 2 || *raw_str.p != '"' || *(raw_str.end - 1) != '"')
        return false;
    size_t klen = strlen(key);
    size_t slen = (size_t)(raw_str.end - raw_str.p - 2);
    return slen == klen && memcmp(raw_str.p + 1, key, klen) == 0;
}

static bool jobj_find(jspan_t obj, const char *key, jspan_t *out) {
    const char *p = skip_ws(obj.p, obj.end);
    if (p >= obj.end || *p != '{') return false;
    ++p;
    while (p < obj.end) {
        p = skip_ws(p, obj.end);
        if (p >= obj.end || *p == '}') return false;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p != '"') return false;
        const char *ks = p;
        const char *ke = skip_string(p, obj.end);
        jspan_t kspan = {ks, ke};
        p = skip_ws(ke, obj.end);
        if (p >= obj.end || *p != ':') return false;
        p = skip_ws(p + 1, obj.end);
        const char *vs = p;
        const char *ve = skip_value(p, obj.end);
        if (jspan_key_eq(kspan, key)) {
            if (out) {
                out->p = vs;
                out->end = ve;
            }
            return true;
        }
        p = ve;
    }
    return false;
}

static bool jspan_is_null(jspan_t s) {
    return (s.end - s.p == 4) && memcmp(s.p, "null", 4) == 0;
}

static void jspan_unquote(jspan_t s, char *dst, size_t dst_cap) {
    if (!dst_cap) return;
    dst[0] = '\0';
    if (s.end - s.p < 2 || *s.p != '"' || *(s.end - 1) != '"') return;
    const char *p = s.p + 1;
    const char *end = s.end - 1;
    size_t o = 0;
    while (p < end && o + 1 < dst_cap) {
        if (*p == '\\' && p + 1 < end) {
            ++p;
            char c = *p++;
            if (c == 'n') dst[o++] = '\n';
            else if (c == 'r') dst[o++] = '\r';
            else if (c == 't') dst[o++] = '\t';
            else if (c == '"' || c == '\\' || c == '/') dst[o++] = c;
            else if (c == 'u' && p + 4 <= end) {
                p += 4;
                dst[o++] = '?';
            } else {
                dst[o++] = c;
            }
        } else {
            dst[o++] = *p++;
        }
    }
    dst[o] = '\0';
}

static bool jobj_get_str(jspan_t obj, const char *key, char *dst, size_t dst_cap) {
    jspan_t v;
    if (!jobj_find(obj, key, &v) || jspan_is_null(v)) return false;
    if (v.end - v.p < 2 || *v.p != '"') return false;
    jspan_unquote(v, dst, dst_cap);
    return true;
}

static int64_t jobj_get_i64(jspan_t obj, const char *key, int64_t def_val) {
    jspan_t v;
    if (!jobj_find(obj, key, &v) || jspan_is_null(v) || v.p >= v.end) return def_val;
    char tmp[64];
    size_t n = (size_t)(v.end - v.p);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, v.p, n);
    tmp[n] = '\0';
    char *ep = NULL;
    long long val = strtoll(tmp, &ep, 10);
    return (ep == tmp) ? def_val : (int64_t)val;
}

static bool jobj_get_double(jspan_t obj, const char *key, double *out) {
    jspan_t v;
    if (!jobj_find(obj, key, &v) || jspan_is_null(v) || v.p >= v.end) return false;
    char tmp[64];
    size_t n = (size_t)(v.end - v.p);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, v.p, n);
    tmp[n] = '\0';
    char *ep = NULL;
    double val = strtod(tmp, &ep);
    if (ep == tmp) return false;
    *out = val;
    return true;
}

static bool parse_iso_ts(const char *s, double *out) {
    if (!s || !*s) return false;
    while (*s == ' ') ++s;
    int Y, M, D, h, m, sec;
    int n = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d%n", &Y, &M, &D, &h, &m, &sec, &n) < 6 &&
        sscanf(s, "%d-%d-%d %d:%d:%d%n", &Y, &M, &D, &h, &m, &sec, &n) < 6) {
        return false;
    }
    const char *p = s + n;
    double frac = 0.0;
    if (*p == '.') {
        ++p;
        double scale = 0.1;
        while (isdigit((unsigned char)*p)) {
            frac += (*p - '0') * scale;
            scale *= 0.1;
            ++p;
        }
    }
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = Y - 1900;
    tm_val.tm_mon = M - 1;
    tm_val.tm_mday = D;
    tm_val.tm_hour = h;
    tm_val.tm_min = m;
    tm_val.tm_sec = sec;

    if (*p == 'Z' || *p == 'z') {
        time_t t = timegm(&tm_val);
        if (t == (time_t)-1) return false;
        *out = (double)t + frac;
        return true;
    }
    if (*p == '+' || *p == '-') {
        int sign = (*p == '-') ? -1 : 1;
        int tzh = 0, tzm = 0;
        if (sscanf(p + 1, "%d:%d", &tzh, &tzm) >= 1) {
            time_t t = timegm(&tm_val);
            if (t == (time_t)-1) return false;
            *out = (double)t - sign * (tzh * 3600 + tzm * 60) + frac;
            return true;
        }
    }
    tm_val.tm_isdst = -1;
    time_t t = mktime(&tm_val);
    if (t == (time_t)-1) return false;
    *out = (double)t + frac;
    return true;
}

static void epoch_to_day(double ts, char out[16]) {
    time_t sec = (time_t)ts;
    struct tm tm_val;
    localtime_r(&sec, &tm_val);
    strftime(out, 16, "%Y-%m-%d", &tm_val);
}

int64_t ai_token_stats_total(const ai_token_stats_t *t) {
    return t->input_tokens + t->cache_read_tokens + t->cache_write_tokens +
           t->output_tokens + t->reasoning_tokens;
}

void ai_token_stats_add(ai_token_stats_t *dst, const ai_token_stats_t *src) {
    dst->turns += src->turns;
    dst->input_tokens += src->input_tokens;
    dst->cache_read_tokens += src->cache_read_tokens;
    dst->cache_write_tokens += src->cache_write_tokens;
    dst->output_tokens += src->output_tokens;
    dst->reasoning_tokens += src->reasoning_tokens;
}

bool ai_quota_elapsed_pct(const ai_quota_window_t *qw, double now, double *out_pct) {
    if (!qw->has_reset || qw->window_seconds <= 0) return false;
    double rem = qw->reset_epoch - now;
    if (rem < 0.0) {
        *out_pct = 100.0;
        return true;
    }
    double pct = 100.0 * (1.0 - rem / (double)qw->window_seconds);
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    *out_pct = pct;
    return true;
}

bool ai_quota_projected_pct(const ai_quota_window_t *qw, double now, double *out_pct) {
    double el = 0.0;
    if (!ai_quota_elapsed_pct(qw, now, &el) || el < 5.0) return false;
    *out_pct = qw->used_pct / (el / 100.0);
    return true;
}

void ai_fmt_tokens(int64_t n, char *buf, size_t cap) {
    if (n >= 1000000000LL) {
        snprintf(buf, cap, "%.2fB", (double)n / 1e9);
    } else if (n >= 1000000LL) {
        snprintf(buf, cap, "%.2fM", (double)n / 1e6);
    } else if (n >= 1000LL) {
        snprintf(buf, cap, "%.1fk", (double)n / 1e3);
    } else {
        snprintf(buf, cap, "%lld", (long long)n);
    }
}

void ai_fmt_duration(double seconds, char *buf, size_t cap) {
    if (seconds <= 0.0) {
        snprintf(buf, cap, "now");
        return;
    }
    int m = (int)(seconds / 60.0);
    int h = m / 60;
    int d = h / 24;
    if (d > 0) {
        snprintf(buf, cap, "%dd %dh", d, h % 24);
    } else if (h > 0) {
        snprintf(buf, cap, "%dh %02dm", h, m % 60);
    } else {
        snprintf(buf, cap, "%dm", m);
    }
}

static ai_token_stats_t *keyed_get_or_add(
    ai_keyed_stats_t **arr, int *n, int *cap, const char *key
) {
    for (int i = 0; i < *n; ++i) {
        if (strcmp((*arr)[i].key, key) == 0) return &(*arr)[i].stats;
    }
    if (*n >= *cap) {
        int ncap = (*cap == 0) ? 16 : (*cap * 2);
        ai_keyed_stats_t *na = realloc(*arr, (size_t)ncap * sizeof(ai_keyed_stats_t));
        if (!na) return NULL;
        *arr = na;
        *cap = ncap;
    }
    ai_keyed_stats_t *ent = &(*arr)[(*n)++];
    memset(ent, 0, sizeof(*ent));
    snprintf(ent->key, sizeof(ent->key), "%s", key);
    return &ent->stats;
}

static ai_session_info_t *session_get_or_add(ai_provider_usage_t *pu, const char *sid) {
    for (int i = 0; i < pu->n_sessions; ++i) {
        if (strcmp(pu->sessions[i].session_id, sid) == 0) return &pu->sessions[i];
    }
    if (pu->n_sessions >= pu->cap_sessions) {
        int ncap = (pu->cap_sessions == 0) ? 32 : (pu->cap_sessions * 2);
        ai_session_info_t *ns = realloc(pu->sessions, (size_t)ncap * sizeof(ai_session_info_t));
        if (!ns) return NULL;
        pu->sessions = ns;
        pu->cap_sessions = ncap;
    }
    ai_session_info_t *s = &pu->sessions[pu->n_sessions++];
    memset(s, 0, sizeof(*s));
    snprintf(s->provider, sizeof(s->provider), "%s", pu->provider);
    snprintf(s->session_id, sizeof(s->session_id), "%s", sid);
    return s;
}

static int64_t record_turn(
    ai_provider_usage_t *pu,
    double ts,
    const char *model,
    int64_t inp,
    int64_t cache_r,
    int64_t cache_w,
    int64_t out,
    int64_t reas
) {
    ai_token_stats_t step = {
        .turns = 1,
        .input_tokens = inp > 0 ? inp : 0,
        .cache_read_tokens = cache_r > 0 ? cache_r : 0,
        .cache_write_tokens = cache_w > 0 ? cache_w : 0,
        .output_tokens = out > 0 ? out : 0,
        .reasoning_tokens = reas > 0 ? reas : 0,
    };
    ai_token_stats_add(&pu->totals, &step);
    const char *mkey = (model && *model) ? model : "unknown";
    ai_token_stats_t *ms = keyed_get_or_add(&pu->by_model, &pu->n_models, &pu->cap_models, mkey);
    if (ms) ai_token_stats_add(ms, &step);
    char dkey[16];
    epoch_to_day(ts, dkey);
    ai_token_stats_t *ds = keyed_get_or_add(&pu->by_day, &pu->n_days, &pu->cap_days, dkey);
    if (ds) ai_token_stats_add(ds, &step);
    return ai_token_stats_total(&step);
}

static sqlite3 *open_sqlite_ro(const char *path) {
    char uri[1024];
    sqlite3 *db = NULL;
    snprintf(uri, sizeof(uri), "file:%s?mode=ro", path);
    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) == SQLITE_OK)
        return db;
    if (db) sqlite3_close(db);
    db = NULL;
    /* Read-only sandbox mounts cannot create -shm files for WAL databases without immutable=1. */
    snprintf(uri, sizeof(uri), "file:%s?immutable=1", path);
    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) == SQLITE_OK)
        return db;
    if (db) sqlite3_close(db);
    return NULL;
}

static void claude_quota(ai_provider_usage_t *pu, const char *base_dir) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%.80s/claude-monitor.sock", xdg);
    } else {
        snprintf(addr.sun_path, sizeof(addr.sun_path), "/run/user/%u/claude-monitor.sock", (unsigned)getuid());
    }

    struct stat st;
    if (stat(addr.sun_path, &st) == 0) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                const char *req = "{\"query\":\"snapshot\"}\n";
                if (send(fd, req, strlen(req), 0) > 0) {
                    size_t cap = 65536;
                    char *buf = malloc(cap);
                    if (buf) {
                        size_t used = 0;
                        while (used + 1 < cap) {
                            ssize_t n = recv(fd, buf + used, cap - 1 - used, 0);
                            if (n <= 0) break;
                            used += (size_t)n;
                            buf[used] = '\0';
                            if (memchr(buf + used - (size_t)n, '\n', (size_t)n)) break;
                        }
                        if (used > 0) {
                            char *nl = strchr(buf, '\n');
                            if (nl) *nl = '\0';
                            jspan_t root = {buf, buf + strlen(buf)};
                            jspan_t usage;
                            if (jobj_find(root, "usage", &usage)) {
                                double upd = 0.0;
                                bool has_upd = jobj_get_double(root, "updated_at", &upd);
                                double pct5 = 0.0;
                                if (jobj_get_double(usage, "used_percentage", &pct5) ||
                                    jobj_get_double(usage, "five_hour_pct", &pct5)) {
                                    ai_quota_window_t *q = &pu->quotas[pu->n_quotas++];
                                    snprintf(q->label, sizeof(q->label), "5h");
                                    q->used_pct = pct5;
                                    q->window_seconds = 5 * 3600;
                                    q->has_reset = jobj_get_double(usage, "resets_at_epoch", &q->reset_epoch) ||
                                                   jobj_get_double(usage, "five_hour_reset", &q->reset_epoch);
                                    q->has_updated = has_upd;
                                    q->updated_epoch = upd;
                                }
                                double pct7 = 0.0;
                                if (jobj_get_double(usage, "seven_day_pct", &pct7)) {
                                    ai_quota_window_t *q = &pu->quotas[pu->n_quotas++];
                                    snprintf(q->label, sizeof(q->label), "7d");
                                    q->used_pct = pct7;
                                    q->window_seconds = 7 * 86400;
                                    q->has_reset = jobj_get_double(usage, "seven_day_reset", &q->reset_epoch);
                                    q->has_updated = has_upd;
                                    q->updated_epoch = upd;
                                }
                                if (pu->n_quotas > 0) {
                                    char mix[64] = "";
                                    if (jobj_get_str(usage, "model_mix", mix, sizeof(mix)) && *mix) {
                                        snprintf(pu->quota_note, sizeof(pu->quota_note), "live daemon (%.42s)", mix);
                                    } else {
                                        snprintf(pu->quota_note, sizeof(pu->quota_note), "live daemon");
                                    }
                                    free(buf);
                                    close(fd);
                                    return;
                                }
                            }
                        }
                        free(buf);
                    }
                }
            }
            close(fd);
        }
    }

    char hist_path[1024];
    snprintf(hist_path, sizeof(hist_path), "%s/usage-history.jsonl", base_dir);
    FILE *f = fopen(hist_path, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            long off = sz > 4096 ? sz - 4096 : 0;
            fseek(f, off, SEEK_SET);
            char buf[4097];
            size_t nr = fread(buf, 1, 4096, f);
            buf[nr] = '\0';
            char *last_line = NULL;
            char *saveptr = NULL;
            for (char *tok = strtok_r(buf, "\r\n", &saveptr); tok; tok = strtok_r(NULL, "\r\n", &saveptr)) {
                if (*tok) last_line = tok;
            }
            if (last_line) {
                jspan_t root = {last_line, last_line + strlen(last_line)};
                double upd = 0.0;
                bool has_upd = jobj_get_double(root, "t", &upd);
                double pct5 = 0.0;
                if (jobj_get_double(root, "pct", &pct5)) {
                    ai_quota_window_t *q = &pu->quotas[pu->n_quotas++];
                    snprintf(q->label, sizeof(q->label), "5h");
                    q->used_pct = pct5;
                    q->window_seconds = 5 * 3600;
                    q->has_reset = jobj_get_double(root, "reset", &q->reset_epoch);
                    q->has_updated = has_upd;
                    q->updated_epoch = upd;
                }
                double pct7 = 0.0;
                if (jobj_get_double(root, "pct7", &pct7)) {
                    ai_quota_window_t *q = &pu->quotas[pu->n_quotas++];
                    snprintf(q->label, sizeof(q->label), "7d");
                    q->used_pct = pct7;
                    q->window_seconds = 7 * 86400;
                    q->has_reset = jobj_get_double(root, "reset7", &q->reset_epoch);
                    q->has_updated = has_upd;
                    q->updated_epoch = upd;
                }
                if (pu->n_quotas > 0) {
                    snprintf(pu->quota_note, sizeof(pu->quota_note), "usage-history.jsonl");
                    fclose(f);
                    return;
                }
            }
        }
        fclose(f);
    }
    snprintf(pu->quota_note, sizeof(pu->quota_note), "unavailable");
}

static void collect_claude(ai_provider_usage_t *pu, const char *base_dir, double cutoff_epoch) {
    snprintf(pu->provider, sizeof(pu->provider), "claude");
    claude_quota(pu, base_dir);

    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s/projects/*/*.jsonl", base_dir);
    glob_t g;
    if (glob(pattern, GLOB_NOSORT, NULL, &g) != 0) return;

    char *line = NULL;
    size_t line_cap = 0;

    for (size_t i = 0; i < g.gl_pathc; ++i) {
        const char *jpath = g.gl_pathv[i];
        struct stat st;
        if (stat(jpath, &st) != 0) continue;
        if (cutoff_epoch > 0.0 && (double)st.st_mtime < cutoff_epoch) continue;

        FILE *f = fopen(jpath, "r");
        if (!f) continue;

        char (*seen_ids)[64] = NULL;
        int n_seen = 0, cap_seen = 0;

        char stem[128] = "";
        char parent_name[256] = "";
        const char *slash = strrchr(jpath, '/');
        if (slash) {
            snprintf(stem, sizeof(stem), "%s", slash + 1);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = '\0';
            const char *pstart = slash;
            while (pstart > jpath && *(pstart - 1) != '/') --pstart;
            size_t plen = (size_t)(slash - pstart);
            if (plen >= sizeof(parent_name)) plen = sizeof(parent_name) - 1;
            memcpy(parent_name, pstart, plen);
            parent_name[plen] = '\0';
        }

        ssize_t llen;
        while ((llen = getline(&line, &line_cap, f)) > 0) {
            if (!strstr(line, "\"assistant\"") && !strstr(line, "\"usage\"")) continue;
            jspan_t root = {line, line + llen};
            char ltype[32];
            if (!jobj_get_str(root, "type", ltype, sizeof(ltype)) || strcmp(ltype, "assistant") != 0)
                continue;
            jspan_t msg, usage;
            if (!jobj_find(root, "message", &msg) || !jobj_find(msg, "usage", &usage))
                continue;

            char mid[64];
            if (jobj_get_str(msg, "id", mid, sizeof(mid)) && *mid) {
                bool dup = false;
                for (int k = 0; k < n_seen; ++k) {
                    if (strcmp(seen_ids[k], mid) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                if (n_seen >= cap_seen) {
                    int ncap = cap_seen ? cap_seen * 2 : 64;
                    void *np = realloc(seen_ids, (size_t)ncap * sizeof(*seen_ids));
                    if (np) {
                        seen_ids = np;
                        cap_seen = ncap;
                    }
                }
                if (n_seen < cap_seen) {
                    snprintf(seen_ids[n_seen++], 64, "%s", mid);
                }
            }

            char ts_str[64];
            double ts = (double)st.st_mtime;
            if (jobj_get_str(root, "timestamp", ts_str, sizeof(ts_str))) {
                parse_iso_ts(ts_str, &ts);
            }
            if (cutoff_epoch > 0.0 && ts < cutoff_epoch) continue;

            char model[128] = "claude";
            jobj_get_str(msg, "model", model, sizeof(model));
            int64_t inp = jobj_get_i64(usage, "input_tokens", 0);
            int64_t cache_r = jobj_get_i64(usage, "cache_read_input_tokens", 0);
            int64_t cache_w = jobj_get_i64(usage, "cache_creation_input_tokens", 0);
            int64_t out = jobj_get_i64(usage, "output_tokens", 0);
            int64_t reas = 0;
            jspan_t details;
            if (jobj_find(usage, "output_tokens_details", &details)) {
                reas = jobj_get_i64(details, "thinking_tokens", 0);
            }

            int64_t tok = record_turn(pu, ts, model, inp, cache_r, cache_w, out, reas);

            char sid[128];
            if (!jobj_get_str(root, "sessionId", sid, sizeof(sid)) || !*sid) {
                snprintf(sid, sizeof(sid), "%s", stem);
            }
            char cwd[512];
            if (!jobj_get_str(root, "cwd", cwd, sizeof(cwd)) || !*cwd) {
                snprintf(cwd, sizeof(cwd), "%s", parent_name);
            }

            ai_session_info_t *s = session_get_or_add(pu, sid);
            if (s) {
                if (s->turns == 0) {
                    s->updated_epoch = ts;
                    snprintf(s->cwd, sizeof(s->cwd), "%s", cwd);
                    snprintf(s->model, sizeof(s->model), "%s", model);
                    s->turns = 1;
                    s->total_tokens = tok;
                } else {
                    s->turns += 1;
                    s->total_tokens += tok;
                    if (ts >= s->updated_epoch) {
                        s->updated_epoch = ts;
                        snprintf(s->model, sizeof(s->model), "%s", model);
                        snprintf(s->cwd, sizeof(s->cwd), "%s", cwd);
                    }
                }
            }
        }
        free(seen_ids);
        fclose(f);
    }
    free(line);
    globfree(&g);
    pu->sessions_count = pu->n_sessions;
}

typedef struct {
    char *path;
    double mtime;
} file_mtime_t;

static void walk_jsonl_files(const char *dir_path, file_mtime_t **list, int *n, int *cap) {
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_jsonl_files(full, list, n, cap);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(de->d_name);
            if (len > 6 && strcmp(de->d_name + len - 6, ".jsonl") == 0) {
                if (*n >= *cap) {
                    int ncap = *cap ? *cap * 2 : 64;
                    file_mtime_t *nl = realloc(*list, (size_t)ncap * sizeof(file_mtime_t));
                    if (!nl) continue;
                    *list = nl;
                    *cap = ncap;
                }
                (*list)[*n].path = strdup(full);
                (*list)[*n].mtime = (double)st.st_mtime;
                ++(*n);
            }
        }
    }
    closedir(d);
}

static int cmp_file_mtime(const void *a, const void *b) {
    const file_mtime_t *fa = (const file_mtime_t *)a;
    const file_mtime_t *fb = (const file_mtime_t *)b;
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    return strcmp(fa->path, fb->path);
}

static void collect_codex(ai_provider_usage_t *pu, const char *base_dir, double cutoff_epoch) {
    snprintf(pu->provider, sizeof(pu->provider), "codex");
    struct stat st;
    if (stat(base_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(pu->quota_note, sizeof(pu->quota_note), "unavailable");
        return;
    }

    file_mtime_t *files = NULL;
    int n_files = 0, cap_files = 0;
    char sub[1024];
    snprintf(sub, sizeof(sub), "%s/sessions", base_dir);
    walk_jsonl_files(sub, &files, &n_files, &cap_files);
    snprintf(sub, sizeof(sub), "%s/archived_sessions", base_dir);
    walk_jsonl_files(sub, &files, &n_files, &cap_files);
    if (n_files > 1) qsort(files, (size_t)n_files, sizeof(file_mtime_t), cmp_file_mtime);

    bool has_rl = false;
    double latest_rl_ts = 0.0;
    char rl_plan[32] = "codex";
    ai_quota_window_t rl_q[2];
    int rl_nq = 0;

    char (*rollout_sids)[128] = NULL;
    int n_rsids = 0, cap_rsids = 0;

    char *line = NULL;
    size_t line_cap = 0;

    for (int i = 0; i < n_files; ++i) {
        bool need_for_turns = (cutoff_epoch <= 0.0) || (files[i].mtime >= cutoff_epoch);
        bool is_recent = (i >= n_files - 5);
        if (!need_for_turns && !is_recent) continue;

        FILE *f = fopen(files[i].path, "r");
        if (!f) continue;

        char sid[128] = "";
        const char *slash = strrchr(files[i].path, '/');
        snprintf(sid, sizeof(sid), "%s", slash ? slash + 1 : files[i].path);
        char *dot = strrchr(sid, '.');
        if (dot) *dot = '\0';

        char cwd[512] = "";
        char cur_model[128] = "codex";
        int64_t prev_totals[5] = {0, 0, 0, 0, 0};

        ssize_t llen;
        while ((llen = getline(&line, &line_cap, f)) > 0) {
            jspan_t root = {line, line + llen};
            char otype[32];
            if (!jobj_get_str(root, "type", otype, sizeof(otype))) continue;
            jspan_t payload;
            if (!jobj_find(root, "payload", &payload)) continue;

            char ts_str[64];
            double ts = files[i].mtime;
            if (jobj_get_str(root, "timestamp", ts_str, sizeof(ts_str))) {
                parse_iso_ts(ts_str, &ts);
            }

            if (strcmp(otype, "session_meta") == 0) {
                if (!jobj_get_str(payload, "session_id", sid, sizeof(sid))) {
                    jobj_get_str(payload, "id", sid, sizeof(sid));
                }
                jobj_get_str(payload, "cwd", cwd, sizeof(cwd));
            } else if (strcmp(otype, "turn_context") == 0) {
                jobj_get_str(payload, "model", cur_model, sizeof(cur_model));
                jobj_get_str(payload, "cwd", cwd, sizeof(cwd));
            } else if (strcmp(otype, "event_msg") == 0) {
                char ptype[32];
                if (!jobj_get_str(payload, "type", ptype, sizeof(ptype)) ||
                    strcmp(ptype, "token_count") != 0) {
                    continue;
                }
                jspan_t rl;
                if (jobj_find(payload, "rate_limits", &rl) && !jspan_is_null(rl) && ts >= latest_rl_ts) {
                    latest_rl_ts = ts;
                    has_rl = true;
                    rl_nq = 0;
                    if (!jobj_get_str(rl, "plan_type", rl_plan, sizeof(rl_plan)) || !*rl_plan) {
                        snprintf(rl_plan, sizeof(rl_plan), "codex");
                    }
                    const char *keys[2] = {"primary", "secondary"};
                    const char *labels[2] = {"5h", "7d"};
                    const int def_wins[2] = {5 * 3600, 7 * 86400};
                    for (int w = 0; w < 2; ++w) {
                        jspan_t win;
                        double upct = 0.0;
                        if (jobj_find(rl, keys[w], &win) && jobj_get_double(win, "used_percent", &upct)) {
                            ai_quota_window_t *qw = &rl_q[rl_nq++];
                            memset(qw, 0, sizeof(*qw));
                            snprintf(qw->label, sizeof(qw->label), "%s", labels[w]);
                            qw->used_pct = upct;
                            int64_t wmin = jobj_get_i64(win, "window_minutes", def_wins[w] / 60);
                            qw->window_seconds = (int)(wmin * 60);
                            qw->has_reset = jobj_get_double(win, "resets_at", &qw->reset_epoch);
                            qw->has_updated = true;
                            qw->updated_epoch = latest_rl_ts;
                        }
                    }
                }

                jspan_t info;
                if (!jobj_find(payload, "info", &info) || jspan_is_null(info)) continue;
                jspan_t tot_u, last_u;
                int64_t inp = 0, cache_r = 0, cache_w = 0, out = 0, reas = 0;
                if (jobj_find(info, "total_token_usage", &tot_u) && !jspan_is_null(tot_u)) {
                    int64_t cur[5] = {
                        jobj_get_i64(tot_u, "input_tokens", 0),
                        jobj_get_i64(tot_u, "cached_input_tokens", 0),
                        jobj_get_i64(tot_u, "cache_write_input_tokens", 0),
                        jobj_get_i64(tot_u, "output_tokens", 0),
                        jobj_get_i64(tot_u, "reasoning_output_tokens", 0),
                    };
                    if (memcmp(cur, prev_totals, sizeof(cur)) == 0) continue;
                    inp = cur[0] > prev_totals[0] ? cur[0] - prev_totals[0] : 0;
                    cache_r = cur[1] > prev_totals[1] ? cur[1] - prev_totals[1] : 0;
                    cache_w = cur[2] > prev_totals[2] ? cur[2] - prev_totals[2] : 0;
                    out = cur[3] > prev_totals[3] ? cur[3] - prev_totals[3] : 0;
                    reas = cur[4] > prev_totals[4] ? cur[4] - prev_totals[4] : 0;
                    memcpy(prev_totals, cur, sizeof(cur));
                } else if (jobj_find(info, "last_token_usage", &last_u) && !jspan_is_null(last_u)) {
                    inp = jobj_get_i64(last_u, "input_tokens", 0);
                    cache_r = jobj_get_i64(last_u, "cached_input_tokens", 0);
                    cache_w = jobj_get_i64(last_u, "cache_write_input_tokens", 0);
                    out = jobj_get_i64(last_u, "output_tokens", 0);
                    reas = jobj_get_i64(last_u, "reasoning_output_tokens", 0);
                } else {
                    continue;
                }

                bool seen_sid = false;
                for (int k = 0; k < n_rsids; ++k) {
                    if (strcmp(rollout_sids[k], sid) == 0) {
                        seen_sid = true;
                        break;
                    }
                }
                if (!seen_sid) {
                    if (n_rsids >= cap_rsids) {
                        int ncap = cap_rsids ? cap_rsids * 2 : 32;
                        void *np = realloc(rollout_sids, (size_t)ncap * sizeof(*rollout_sids));
                        if (np) {
                            rollout_sids = np;
                            cap_rsids = ncap;
                        }
                    }
                    if (n_rsids < cap_rsids) {
                        snprintf(rollout_sids[n_rsids++], 128, "%s", sid);
                    }
                }

                if (cutoff_epoch > 0.0 && ts < cutoff_epoch) continue;
                /* Codex input_tokens includes cached_input_tokens; subtract to keep total exact. */
                int64_t non_cached_inp = inp > cache_r ? inp - cache_r : 0;
                int64_t tok = record_turn(pu, ts, cur_model, non_cached_inp, cache_r, cache_w, out, reas);

                ai_session_info_t *s = session_get_or_add(pu, sid);
                if (s) {
                    if (s->turns == 0) {
                        s->updated_epoch = ts;
                        snprintf(s->cwd, sizeof(s->cwd), "%s", cwd);
                        snprintf(s->model, sizeof(s->model), "%s", cur_model);
                        s->turns = 1;
                        s->total_tokens = tok;
                    } else {
                        s->turns += 1;
                        s->total_tokens += tok;
                        if (ts >= s->updated_epoch) {
                            s->updated_epoch = ts;
                            snprintf(s->model, sizeof(s->model), "%s", cur_model);
                            if (*cwd) snprintf(s->cwd, sizeof(s->cwd), "%s", cwd);
                        }
                    }
                }
            }
        }
        fclose(f);
    }
    free(line);
    for (int i = 0; i < n_files; ++i) free(files[i].path);
    free(files);

    if (has_rl) {
        snprintf(pu->quota_note, sizeof(pu->quota_note), "plan=%s", rl_plan);
        for (int i = 0; i < rl_nq; ++i) pu->quotas[pu->n_quotas++] = rl_q[i];
    } else {
        snprintf(pu->quota_note, sizeof(pu->quota_note), "no rate_limits in rollouts");
    }

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/state_5.sqlite", base_dir);
    if (stat(db_path, &st) == 0 && S_ISREG(st.st_mode)) {
        sqlite3 *db = open_sqlite_ro(db_path);
        if (db) {
            sqlite3_stmt *stmt = NULL;
            const char *sql = "SELECT id, updated_at, cwd, title, tokens_used, model FROM threads";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *tid = (const char *)sqlite3_column_text(stmt, 0);
                    double upd = sqlite3_column_double(stmt, 1);
                    const char *cwd = (const char *)sqlite3_column_text(stmt, 2);
                    const char *title = (const char *)sqlite3_column_text(stmt, 3);
                    int64_t tok_used = sqlite3_column_int64(stmt, 4);
                    const char *model = (const char *)sqlite3_column_text(stmt, 5);
                    if (!tid || !*tid) continue;
                    if (upd > 1e11) upd /= 1000.0;

                    ai_session_info_t *existing = NULL;
                    for (int i = 0; i < pu->n_sessions; ++i) {
                        if (strcmp(pu->sessions[i].session_id, tid) == 0) {
                            existing = &pu->sessions[i];
                            break;
                        }
                    }
                    if (existing) {
                        if (title && *title) snprintf(existing->title, sizeof(existing->title), "%s", title);
                        if (model && *model && strcmp(existing->model, "codex") == 0) {
                            snprintf(existing->model, sizeof(existing->model), "%s", model);
                        }
                        continue;
                    }
                    bool in_rollout = false;
                    for (int k = 0; k < n_rsids; ++k) {
                        if (strcmp(rollout_sids[k], tid) == 0) {
                            in_rollout = true;
                            break;
                        }
                    }
                    if (in_rollout) continue;
                    if (cutoff_epoch > 0.0 && upd < cutoff_epoch) continue;
                    if (tok_used <= 0) continue;
                    const char *mname = (model && *model) ? model : "codex";
                    record_turn(pu, upd, mname, tok_used, 0, 0, 0, 0);
                    ai_session_info_t *ns = session_get_or_add(pu, tid);
                    if (ns) {
                        ns->updated_epoch = upd;
                        snprintf(ns->cwd, sizeof(ns->cwd), "%s", cwd ? cwd : "");
                        snprintf(ns->model, sizeof(ns->model), "%s", mname);
                        ns->turns = 1;
                        ns->total_tokens = tok_used;
                        snprintf(ns->title, sizeof(ns->title), "%s", title ? title : "");
                    }
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }
    }
    free(rollout_sids);
    pu->sessions_count = pu->n_sessions;
}

typedef struct {
    char cid[128];
    char title[256];
    char cwd[512];
} agy_summary_t;

static void collect_antigravity(
    ai_provider_usage_t *pu,
    const char *base_dir,
    double cutoff_epoch,
    double now_epoch
) {
    snprintf(pu->provider, sizeof(pu->provider), "antigravity");
    snprintf(pu->quota_note, sizeof(pu->quota_note), "local transcripts only");
    struct stat st;
    if (stat(base_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return;

    int64_t tok_5h = 0, turns_5h = 0;
    int64_t tok_7d = 0, turns_7d = 0;
    double oldest_5h = 0.0, oldest_7d = 0.0;
    double cut_5h = now_epoch - 5.0 * 3600.0;
    double cut_7d = now_epoch - 7.0 * 86400.0;
    double scan_cutoff = cutoff_epoch;
    if (scan_cutoff > 0.0 && cut_7d < scan_cutoff) {
        scan_cutoff = cut_7d;
    }

    agy_summary_t *sums = NULL;
    int n_sums = 0, cap_sums = 0;
    const char *db_rels[2] = {
        "antigravity-cli/conversation_summaries.db",
        "antigravity/conversation_summaries.db"
    };
    for (int d = 0; d < 2; ++d) {
        char db_path[1024];
        snprintf(db_path, sizeof(db_path), "%s/%s", base_dir, db_rels[d]);
        if (stat(db_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        sqlite3 *db = open_sqlite_ro(db_path);
        if (!db) continue;
        sqlite3_stmt *stmt = NULL;
        const char *sql = "SELECT conversation_id, title, workspace_uris FROM conversation_summaries";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *cid = (const char *)sqlite3_column_text(stmt, 0);
                const char *title = (const char *)sqlite3_column_text(stmt, 1);
                const char *ws = (const char *)sqlite3_column_text(stmt, 2);
                if (!cid || !*cid) continue;
                int idx = -1;
                for (int k = 0; k < n_sums; ++k) {
                    if (strcmp(sums[k].cid, cid) == 0) {
                        idx = k;
                        break;
                    }
                }
                if (idx < 0) {
                    if (n_sums >= cap_sums) {
                        int ncap = cap_sums ? cap_sums * 2 : 32;
                        agy_summary_t *ns = realloc(sums, (size_t)ncap * sizeof(agy_summary_t));
                        if (!ns) continue;
                        sums = ns;
                        cap_sums = ncap;
                    }
                    idx = n_sums++;
                    snprintf(sums[idx].cid, sizeof(sums[idx].cid), "%s", cid);
                }
                snprintf(sums[idx].title, sizeof(sums[idx].title), "%s", title ? title : "");
                snprintf(sums[idx].cwd, sizeof(sums[idx].cwd), "%s", ws ? ws : "");
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    char pattern[1024];
    snprintf(
        pattern,
        sizeof(pattern),
        "%s/antigravity*/brain/*/.system_generated/logs/transcript.jsonl",
        base_dir
    );
    glob_t g;
    if (glob(pattern, 0, NULL, &g) == 0) {
        char *line = NULL;
        size_t line_cap = 0;
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            const char *tpath = g.gl_pathv[i];
            if (stat(tpath, &st) != 0) continue;
            if (scan_cutoff > 0.0 && (double)st.st_mtime < scan_cutoff) continue;

            char cid[128] = "";
            const char *bpos = strstr(tpath, "/brain/");
            if (bpos) {
                const char *cs = bpos + 7;
                const char *ce = strchr(cs, '/');
                if (ce) {
                    size_t clen = (size_t)(ce - cs);
                    if (clen >= sizeof(cid)) clen = sizeof(cid) - 1;
                    memcpy(cid, cs, clen);
                    cid[clen] = '\0';
                }
            }
            const char *title = "";
            const char *cwd = "";
            for (int k = 0; k < n_sums; ++k) {
                if (strcmp(sums[k].cid, cid) == 0) {
                    title = sums[k].title;
                    cwd = sums[k].cwd;
                    break;
                }
            }

            FILE *f = fopen(tpath, "r");
            if (!f) continue;
            char cur_model[128] = "agy-default";
            ssize_t llen;
            while ((llen = getline(&line, &line_cap, f)) > 0) {
                jspan_t root = {line, line + llen};
                char ltype[32];
                if (!jobj_get_str(root, "type", ltype, sizeof(ltype))) continue;
                if (strcmp(ltype, "USER_INPUT") == 0 && strstr(line, "Model Selection")) {
                    char content[4096];
                    if (jobj_get_str(root, "content", content, sizeof(content))) {
                        const char *ms = strstr(content, "Model Selection");
                        if (ms) {
                            const char *to_p = strstr(ms, " to ");
                            if (to_p) {
                                to_p += 4;
                                const char *le = strpbrk(to_p, ".\r\n");
                                size_t mlen = le ? (size_t)(le - to_p) : strlen(to_p);
                                if (mlen > 0 && mlen < sizeof(cur_model)) {
                                    memcpy(cur_model, to_p, mlen);
                                    cur_model[mlen] = '\0';
                                }
                            }
                        }
                    }
                    continue;
                }
                if (strcmp(ltype, "PLANNER_RESPONSE") != 0) continue;
                char ts_str[64];
                double ts = (double)st.st_mtime;
                if (jobj_get_str(root, "created_at", ts_str, sizeof(ts_str))) {
                    parse_iso_ts(ts_str, &ts);
                }
                int64_t inp = jobj_get_i64(root, "input_tokens", 0);
                int64_t cache_r = jobj_get_i64(root, "cache_read_tokens", 0);
                int64_t out = jobj_get_i64(root, "output_tokens", 0);
                if (inp == 0 && cache_r == 0 && out == 0) continue;

                int64_t turn_tok = inp + cache_r + out;
                if (ts >= cut_5h) {
                    tok_5h += turn_tok;
                    turns_5h += 1;
                    if (oldest_5h == 0.0 || ts < oldest_5h) oldest_5h = ts;
                }
                if (ts >= cut_7d) {
                    tok_7d += turn_tok;
                    turns_7d += 1;
                    if (oldest_7d == 0.0 || ts < oldest_7d) oldest_7d = ts;
                }

                if (cutoff_epoch > 0.0 && ts < cutoff_epoch) continue;

                int64_t tok = record_turn(pu, ts, cur_model, inp, cache_r, 0, out, 0);
                ai_session_info_t *s = session_get_or_add(pu, cid);
                if (s) {
                    if (s->turns == 0) {
                        s->updated_epoch = ts;
                        snprintf(s->cwd, sizeof(s->cwd), "%s", cwd);
                        snprintf(s->model, sizeof(s->model), "%s", cur_model);
                        s->turns = 1;
                        s->total_tokens = tok;
                        snprintf(s->title, sizeof(s->title), "%s", title);
                    } else {
                        s->turns += 1;
                        s->total_tokens += tok;
                        if (ts >= s->updated_epoch) {
                            s->updated_epoch = ts;
                            snprintf(s->model, sizeof(s->model), "%s", cur_model);
                        }
                    }
                }
            }
            fclose(f);
        }
        free(line);
        globfree(&g);
    }
    free(sums);
    pu->sessions_count = pu->n_sessions;

    /* Populate 5h and 7d rolling token windows (scaled vs 50M / 250M reference). */
    ai_quota_window_t *q5 = &pu->quotas[pu->n_quotas++];
    memset(q5, 0, sizeof(*q5));
    snprintf(q5->label, sizeof(q5->label), "5h");
    q5->window_seconds = 5 * 3600;
    q5->is_token_window = true;
    q5->window_tokens = tok_5h;
    q5->window_turns = turns_5h;
    q5->used_pct = ((double)tok_5h / 50e6) * 100.0;
    if (q5->used_pct > 100.0) q5->used_pct = 100.0;
    if (oldest_5h > 0.0) {
        q5->has_reset = true;
        q5->reset_epoch = oldest_5h + 5.0 * 3600.0;
    }
    q5->has_updated = true;
    q5->updated_epoch = now_epoch;

    ai_quota_window_t *q7 = &pu->quotas[pu->n_quotas++];
    memset(q7, 0, sizeof(*q7));
    snprintf(q7->label, sizeof(q7->label), "7d");
    q7->window_seconds = 7 * 86400;
    q7->is_token_window = true;
    q7->window_tokens = tok_7d;
    q7->window_turns = turns_7d;
    q7->used_pct = ((double)tok_7d / 250e6) * 100.0;
    if (q7->used_pct > 100.0) q7->used_pct = 100.0;
    if (oldest_7d > 0.0) {
        q7->has_reset = true;
        q7->reset_epoch = oldest_7d + 7.0 * 86400.0;
    }
    q7->has_updated = true;
    q7->updated_epoch = now_epoch;

    snprintf(
        pu->quota_note,
        sizeof(pu->quota_note),
        "rolling tokens (no server %% cap exposed)"
    );
}

static int cmp_session_desc(const void *a, const void *b) {
    const ai_session_info_t *sa = (const ai_session_info_t *)a;
    const ai_session_info_t *sb = (const ai_session_info_t *)b;
    if (sa->updated_epoch > sb->updated_epoch) return -1;
    if (sa->updated_epoch < sb->updated_epoch) return 1;
    return 0;
}

static int cmp_model_desc(const void *a, const void *b) {
    const ai_keyed_stats_t *ma = (const ai_keyed_stats_t *)a;
    const ai_keyed_stats_t *mb = (const ai_keyed_stats_t *)b;
    int64_t ta = ai_token_stats_total(&ma->stats);
    int64_t tb = ai_token_stats_total(&mb->stats);
    if (ta > tb) return -1;
    if (ta < tb) return 1;
    return strcmp(ma->key, mb->key);
}

static int cmp_day_desc(const void *a, const void *b) {
    const ai_keyed_stats_t *da = (const ai_keyed_stats_t *)a;
    const ai_keyed_stats_t *db = (const ai_keyed_stats_t *)b;
    return strcmp(db->key, da->key);
}

void ai_usage_collect(
    ai_usage_report_t *out,
    int days,
    int provider_mask,
    double now_override,
    const char *claude_dir,
    const char *codex_dir,
    const char *gemini_dir
) {
    memset(out, 0, sizeof(*out));
    double cur_now = now_override > 0.0 ? now_override : (double)time(NULL);
    double cutoff = (days > 0) ? (cur_now - (double)days * 86400.0) : 0.0;
    out->generated_at = cur_now;
    out->days = days > 0 ? days : 0;

    const char *home = getenv("HOME");
    if (!home) home = "";
    char def_claude[512], def_codex[512], def_gemini[512];
    snprintf(def_claude, sizeof(def_claude), "%s/.claude", home);
    snprintf(def_codex, sizeof(def_codex), "%s/.codex", home);
    snprintf(def_gemini, sizeof(def_gemini), "%s/.gemini", home);

    if (provider_mask & AI_PROVIDER_CLAUDE) {
        collect_claude(
            &out->providers[out->n_providers++],
            (claude_dir && *claude_dir) ? claude_dir : def_claude,
            cutoff
        );
    }
    if (provider_mask & AI_PROVIDER_CODEX) {
        collect_codex(
            &out->providers[out->n_providers++],
            (codex_dir && *codex_dir) ? codex_dir : def_codex,
            cutoff
        );
    }
    if (provider_mask & AI_PROVIDER_ANTIGRAVITY) {
        collect_antigravity(
            &out->providers[out->n_providers++],
            (gemini_dir && *gemini_dir) ? gemini_dir : def_gemini,
            cutoff,
            cur_now
        );
    }

    for (int i = 0; i < out->n_providers; ++i) {
        ai_provider_usage_t *pu = &out->providers[i];
        if (pu->n_models > 1) {
            qsort(pu->by_model, (size_t)pu->n_models, sizeof(ai_keyed_stats_t), cmp_model_desc);
        }
        if (pu->n_days > 1) {
            qsort(pu->by_day, (size_t)pu->n_days, sizeof(ai_keyed_stats_t), cmp_day_desc);
        }
        if (pu->n_sessions > 1) {
            qsort(pu->sessions, (size_t)pu->n_sessions, sizeof(ai_session_info_t), cmp_session_desc);
        }
    }
}

void ai_usage_report_free(ai_usage_report_t *r) {
    if (!r) return;
    for (int i = 0; i < r->n_providers; ++i) {
        free(r->providers[i].by_model);
        free(r->providers[i].by_day);
        free(r->providers[i].sessions);
    }
    memset(r, 0, sizeof(*r));
}

static void write_stats_json(strbuf_t *sb, const ai_token_stats_t *t) {
    sb_printf(
        sb,
        "{\"turns\":%lld,\"input_tokens\":%lld,\"cache_read_tokens\":%lld,"
        "\"cache_write_tokens\":%lld,\"output_tokens\":%lld,\"reasoning_tokens\":%lld}",
        (long long)t->turns,
        (long long)t->input_tokens,
        (long long)t->cache_read_tokens,
        (long long)t->cache_write_tokens,
        (long long)t->output_tokens,
        (long long)t->reasoning_tokens
    );
}

char *ai_usage_report_to_json(const ai_usage_report_t *r) {
    strbuf_t sb;
    sb_init(&sb, 16384);
    sb_printf(&sb, "{\"generated_at\":%.3f,\"days\":", r->generated_at);
    if (r->days > 0) sb_printf(&sb, "%d", r->days);
    else sb_puts(&sb, "null");
    sb_puts(&sb, ",\"providers\":[");

    for (int i = 0; i < r->n_providers; ++i) {
        const ai_provider_usage_t *pu = &r->providers[i];
        if (i > 0) sb_puts(&sb, ",");
        sb_puts(&sb, "{\"provider\":");
        sb_json_str(&sb, pu->provider);
        sb_puts(&sb, ",\"quotas\":[");
        for (int q = 0; q < pu->n_quotas; ++q) {
            const ai_quota_window_t *qw = &pu->quotas[q];
            if (q > 0) sb_puts(&sb, ",");
            sb_puts(&sb, "{\"label\":");
            sb_json_str(&sb, qw->label);
            sb_printf(&sb, ",\"used_pct\":%.4f,\"window_seconds\":%d,\"reset_epoch\":", qw->used_pct, qw->window_seconds);
            if (qw->has_reset) sb_printf(&sb, "%.3f", qw->reset_epoch);
            else sb_puts(&sb, "null");
            sb_puts(&sb, ",\"updated_epoch\":");
            if (qw->has_updated) sb_printf(&sb, "%.3f", qw->updated_epoch);
            else sb_puts(&sb, "null");
            sb_puts(&sb, "}");
        }
        sb_puts(&sb, "],\"quota_note\":");
        sb_json_str(&sb, pu->quota_note);
        sb_printf(&sb, ",\"sessions_count\":%d,\"totals\":", pu->sessions_count);
        write_stats_json(&sb, &pu->totals);

        sb_puts(&sb, ",\"by_model\":{");
        for (int m = 0; m < pu->n_models; ++m) {
            if (m > 0) sb_puts(&sb, ",");
            sb_json_str(&sb, pu->by_model[m].key);
            sb_puts(&sb, ":");
            write_stats_json(&sb, &pu->by_model[m].stats);
        }
        sb_puts(&sb, "},\"by_day\":{");
        for (int d = 0; d < pu->n_days; ++d) {
            if (d > 0) sb_puts(&sb, ",");
            sb_json_str(&sb, pu->by_day[d].key);
            sb_puts(&sb, ":");
            write_stats_json(&sb, &pu->by_day[d].stats);
        }
        sb_puts(&sb, "},\"recent_sessions\":[");
        int lim = pu->n_sessions < 15 ? pu->n_sessions : 15;
        for (int s = 0; s < lim; ++s) {
            const ai_session_info_t *si = &pu->sessions[s];
            if (s > 0) sb_puts(&sb, ",");
            sb_puts(&sb, "{\"provider\":");
            sb_json_str(&sb, si->provider);
            sb_puts(&sb, ",\"session_id\":");
            sb_json_str(&sb, si->session_id);
            sb_printf(&sb, ",\"updated_epoch\":%.3f,\"cwd\":", si->updated_epoch);
            sb_json_str(&sb, si->cwd);
            sb_puts(&sb, ",\"model\":");
            sb_json_str(&sb, si->model);
            sb_printf(&sb, ",\"turns\":%lld,\"total_tokens\":%lld,\"title\":", (long long)si->turns, (long long)si->total_tokens);
            sb_json_str(&sb, si->title);
            sb_puts(&sb, "}");
        }
        sb_puts(&sb, "]}");
    }
    sb_puts(&sb, "]}");
    return sb.buf;
}

char *ai_usage_collect_json(
    int days,
    int provider_mask,
    double now_override,
    const char *claude_dir,
    const char *codex_dir,
    const char *gemini_dir
) {
    ai_usage_report_t rep;
    ai_usage_collect(&rep, days, provider_mask, now_override, claude_dir, codex_dir, gemini_dir);
    char *json = ai_usage_report_to_json(&rep);
    ai_usage_report_free(&rep);
    return json;
}

void ai_usage_free_string(char *s) {
    free(s);
}
