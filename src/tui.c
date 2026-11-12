#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#define _POSIX_C_SOURCE 200809L
#include "ai_usage/core.h"

#include <curses.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <langinfo.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    bool stop, requested, busy, ready;
    int days;
    ai_usage_report_t result;
} collector_t;

typedef struct {
    int offset, line, height, width;
} canvas_t;

static volatile sig_atomic_t interrupted;
static const char *providers[] = {"all", "claude", "codex", "antigravity", "opencode"};
#define N_PROVIDERS ((int)(sizeof(providers) / sizeof(providers[0])))
static const char *views[] = {"Overview", "Models", "Daily", "Sessions"};

static void on_signal(int signum) {
    (void)signum;
    interrupted = 1;
}

static void *collect_worker(void *arg) {
    collector_t *c = arg;
    pthread_mutex_lock(&c->mutex);
    while (!c->stop) {
        while (!c->requested && !c->stop) pthread_cond_wait(&c->wake, &c->mutex);
        if (c->stop) break;
        int days = c->days;
        c->requested = false;
        c->busy = true;
        pthread_mutex_unlock(&c->mutex);
        ai_usage_report_t result;
        ai_usage_collect(&result, days, AI_PROVIDER_ALL, 0.0, NULL, NULL, NULL, NULL);
        pthread_mutex_lock(&c->mutex);
        if (c->ready) ai_usage_report_free(&c->result);
        c->result = result;
        c->ready = true;
        c->busy = false;
    }
    pthread_mutex_unlock(&c->mutex);
    return NULL;
}

static void request_collect(collector_t *c, int days) {
    pthread_mutex_lock(&c->mutex);
    c->days = days;
    c->requested = true;
    pthread_cond_signal(&c->wake);
    pthread_mutex_unlock(&c->mutex);
}

enum { MUTED = 1, CLAUDE, CODEX, ANTIGRAVITY, WARNING, DANGER, HEADING, TRACK, TAB, OPENCODE };
static bool unicode_ui, color_ui;
static short saved_palette[10][3];
static bool palette_changed[10];

static void init_theme(void) {
    start_color();
    int background = use_default_colors() == OK ? -1 : COLOR_BLACK;
    const int rgb[][3] = {
        {166, 173, 200}, {250, 179, 135}, {137, 180, 250},
        {166, 227, 161}, {249, 226, 175}, {243, 139, 168},
        {205, 214, 244}, {108, 112, 134}, {49, 50, 68},
        {203, 147, 245}
    };
    short colors[] = {146, 216, 111, 151, 223, 211, 189, 242, 236, 181};
    if (COLORS >= 256 && can_change_color()) {
        for (int i = 0; i < 10; ++i) {
            short slot = (short)(240 + i);
            if (color_content(slot, &saved_palette[i][0], &saved_palette[i][1], &saved_palette[i][2]) == OK &&
                init_color(slot, (short)(rgb[i][0] * 1000 / 255),
                           (short)(rgb[i][1] * 1000 / 255), (short)(rgb[i][2] * 1000 / 255)) == OK) {
                palette_changed[i] = true;
                colors[i] = slot;
            }
        }
    } else if (COLORS < 256) {
        short basic[] = {COLOR_WHITE, COLOR_YELLOW, COLOR_BLUE, COLOR_GREEN,
                         COLOR_YELLOW, COLOR_RED, COLOR_WHITE, COLOR_WHITE, COLOR_BLACK,
                         COLOR_MAGENTA};
        memcpy(colors, basic, sizeof(colors));
        if (background == -1) colors[0] = colors[6] = -1;
    }
    for (int i = 0; i < 8; ++i) init_pair((short)(i + 1), colors[i], (short)background);
    init_pair(TAB, colors[2], colors[8]);
    init_pair(OPENCODE, colors[9], (short)background);
}

static void restore_theme(void) {
    for (int i = 0; i < 10; ++i) if (palette_changed[i])
        init_color((short)(240 + i), saved_palette[i][0], saved_palette[i][1], saved_palette[i][2]);
}

static int provider_color(const char *name) {
    if (!strcmp(name, "claude")) return CLAUDE;
    if (!strcmp(name, "codex")) return CODEX;
    if (!strcmp(name, "opencode")) return OPENCODE;
    return ANTIGRAVITY;
}

static void style(int color, int attributes) {
    if (color_ui && COLORS < 256) attributes &= ~A_BOLD;
    if (color == TRACK && (!color_ui || COLORS < 256)) attributes |= A_DIM;
    attrset(attributes | (color_ui ? COLOR_PAIR(color) : 0));
}

static void put_text(int row, int col, int width, const char *text) {
    if (width <= 0 || row < 0 || row >= LINES || col < 0 || col >= COLS) return;
    int limit = width < COLS - col ? width : COLS - col;
    move(row, col);
    mbstate_t state = {0};
    int cells = 0;
    while (*text && cells < limit) {
        wchar_t ch;
        size_t bytes = mbrtowc(&ch, text, MB_CUR_MAX, &state);
        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            memset(&state, 0, sizeof(state));
            ch = L'?';
            bytes = 1;
        } else if (!bytes) break;
        int n = wcwidth(ch);
        if (!iswprint(ch) || n < 0 || (n == 0 && cells == 0)) { ch = L'?'; n = 1; }
        if (cells + n > limit) break;
        wchar_t out[2] = {ch, 0};
        addwstr(out);
        cells += n;
        text += bytes;
    }
}

static void safe_print(int row, int width, const char *text) {
    put_text(row, 0, width - 1, text);
}

static void at(canvas_t *c, int logical_row, int col, int width, const char *text) {
    int row = logical_row - c->offset;
    if (row >= 0 && row < c->height) put_text(row + 4, col, width, text);
}

static void line(canvas_t *c, const char *format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    at(c, c->line++, 2, c->width - 4, text);
}

static bool selected(const ai_provider_usage_t *p, int provider) {
    return provider == 0 || strcmp(p->provider, providers[provider]) == 0;
}

static void cell(canvas_t *c, int row, int x, int width, const char *text) {
    at(c, row, x, width, text);
}

static void stats_line(canvas_t *c, const char *provider, const char *key,
                       const ai_token_stats_t *s) {
    int row = c->line++, key_width = c->width >= 135 ? c->width - 94 : c->width - 56;
    if (key_width > 40) key_width = 40;
    if (key_width < 10) key_width = 10;
    bool total = !strcmp(provider, "TOTAL");
    style(total ? HEADING : provider_color(provider), total ? A_BOLD : A_NORMAL);
    cell(c, row, 2, 12, provider);
    style(total ? HEADING : MUTED, total ? A_BOLD : A_NORMAL);
    cell(c, row, 15, key_width, key);
    int x = 16 + key_width;
    char text[32];
    snprintf(text, sizeof(text), "%7lld", (long long)s->turns);
    cell(c, row, x, 7, text); x += 9;
    int64_t compact[] = {ai_token_stats_total(s), s->input_tokens, s->output_tokens + s->reasoning_tokens};
    int64_t full[] = {s->input_tokens, s->cache_read_tokens, s->cache_write_tokens,
                      s->output_tokens, s->reasoning_tokens, ai_token_stats_total(s)};
    int count = c->width >= 135 ? 6 : 3;
    for (int i = 0; i < count; ++i) {
        char value[24];
        ai_fmt_tokens(c->width >= 135 ? full[i] : compact[i], value, sizeof(value));
        snprintf(text, sizeof(text), "%9s", value);
        style(total || i == (count == 3 ? 0 : 5) ? HEADING : MUTED,
              total ? A_BOLD : A_NORMAL);
        cell(c, row, x, 9, text); x += 10;
    }
    style(MUTED, A_NORMAL);
}

static void stats_header(canvas_t *c, const char *key) {
    int key_width = c->width >= 135 ? c->width - 94 : c->width - 56;
    if (key_width > 40) key_width = 40;
    if (key_width < 10) key_width = 10;
    int row = c->line++;
    style(HEADING, A_BOLD);
    cell(c, row, 2, 12, "Provider"); cell(c, row, 15, key_width, key);
    cell(c, row, 16 + key_width, c->width, c->width >= 135 ?
         "  Turns      Input CacheRead   CacheWr    Output Reasoning     Total" :
         "  Turns      Total     Input  Out+Reas");
    style(MUTED, A_NORMAL);
    line(c, "");
}

static void card_border(canvas_t *c, int row, int x, int width, bool top) {
    const char *left = unicode_ui ? (top ? "╭" : "╰") : "+";
    const char *right = unicode_ui ? (top ? "╮" : "╯") : "+";
    cell(c, row, x, 1, left);
    for (int j = 1; j < width - 1; ++j) cell(c, row, x + j, 1, unicode_ui ? "─" : "-");
    cell(c, row, x + width - 1, 1, right);
}

static void card_text(canvas_t *c, int row, int x, int width, const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
    cell(c, row, x + 2, width - 4, text);
}

static void quota_card(canvas_t *c, const ai_provider_usage_t *p, int row,
                       int x, int width, int height, double now) {
    int accent = provider_color(p->provider);
    style(TRACK, A_NORMAL);
    card_border(c, row, x, width, true);
    for (int j = 1; j < height - 1; ++j) {
        cell(c, row + j, x, 1, unicode_ui ? "│" : "|");
        cell(c, row + j, x + width - 1, 1, unicode_ui ? "│" : "|");
    }
    card_border(c, row + height - 1, x, width, false);
    style(accent, A_NORMAL); card_text(c, row + 1, x, width, "%s", p->provider);
    char tokens[32]; ai_fmt_tokens(ai_token_stats_total(&p->totals), tokens, sizeof(tokens));
    style(HEADING, A_BOLD);
    card_text(c, row + 2, x, width, "%s tokens  /  %d sessions", tokens, p->sessions_count);
    bool local = (!strcmp(p->provider, "antigravity") && !strncmp(p->quota_note, "rolling window", 14)) ||
        (p->n_quotas > 0 && p->quotas[0].is_token_window);
    style(MUTED, A_NORMAL);
    card_text(c, row + 3, x, width, "%s", !p->n_quotas ? "No quota" : local ? "Local rolling usage" : "Quota / rate limits");
    int y = row + 5;
    if (!p->n_quotas) {
        card_text(c, y++, x, width, "No quota reading");
        card_text(c, y, x, width, "%s", p->quota_note[0] ? p->quota_note : "unavailable");
    }
    for (int i = 0; i < p->n_quotas; ++i) {
        const ai_quota_window_t *q = &p->quotas[i];
        char reset[64] = "unknown", projection[64] = "unavailable";
        if (q->has_reset) ai_fmt_duration(q->reset_epoch - now, reset, sizeof(reset));
        bool token_window = local || q->is_token_window;
        double projected = 0, elapsed = 0;
        bool expired = q->has_reset && q->reset_epoch <= now;
        bool has_projection = !expired && (!q->from_history || q->has_updated) &&
            ai_quota_projected_pct(q, q->has_updated ? fmin(q->updated_epoch, now) : now, &projected);
        if (token_window) {
            int64_t projected_tokens;
            ai_quota_window_t copy = *q; copy.is_token_window = true;
            ai_fmt_tokens(q->window_tokens, tokens, sizeof(tokens));
            if (ai_quota_projected_tokens(&copy, now, &projected_tokens))
                ai_fmt_tokens(projected_tokens, projection, sizeof(projection));
            style(accent, A_NORMAL);
            card_text(c, y++, x, width, "%s  %s tokens / %lld turns", q->label, tokens, (long long)q->window_turns);
            style(MUTED, A_NORMAL);
            card_text(c, y++, x, width, "Projected tokens: %s", projection);
        } else {
            int severity = has_projection && projected > 100 ? DANGER : MUTED;
            style(HEADING, A_NORMAL);
            card_text(c, y++, x, width, "%s  %.1f%%%s", q->label, q->used_pct, expired ? " awaiting update" : "");
            int bar_width = width - 6;
            double fill = isfinite(q->used_pct) ? fmax(0, fmin(100, q->used_pct)) * bar_width / 100 : 0;
            bool has_elapsed = ai_quota_elapsed_pct(q, now, &elapsed) && isfinite(elapsed);
            int marker = has_elapsed ? (int)(fmax(0, fmin(100, elapsed)) * (bar_width - 1) / 100) : -1;
            const char *blocks[] = {" ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};
            for (int j = 0; j < bar_width; ++j) {
                int fraction = (int)lround(fmax(0, fmin(1, fill - j)) * 8);
                style(j == marker ? MUTED : fraction ? (q->used_pct >= 90 ? WARNING : accent) : TRACK, A_NORMAL);
                cell(c, y, x + 3 + j, 1, j == marker ? (unicode_ui ? "│" : "|") : unicode_ui ? (fraction ? blocks[fraction] : "─") : (fraction ? "#" : "."));
            }
            ++y;
            if (has_projection) snprintf(projection, sizeof(projection), "%.0f%%", projected);
            style(severity, A_NORMAL);
            card_text(c, y++, x, width, "Projected: %s", projection);
        }
        style(MUTED, A_NORMAL);
        card_text(c, y++, x, width, "Reset: %s", reset);
        card_text(c, y++, x, width, "Source: %s", token_window ? "local logs" : q->from_history ? "saved history" : p->quota_note[0] ? p->quota_note : "provider reading");
        if (q->from_history) card_text(c, y, x, width, "Last recorded reading");
        ++y;
    }
    style(MUTED, A_NORMAL);
}

static int render_body(canvas_t *c, const ai_usage_report_t *r, int view, int provider) {
    style(MUTED, A_NORMAL);
    if (!r->generated_at) {
        line(c, "Loading usage... Navigation remains available."); return c->line;
    }
    int rows = 0, sessions = 0, count = 0;
    ai_token_stats_t total = {0};
    const ai_provider_usage_t *chosen[4];
    for (int i = 0; i < r->n_providers; ++i) if (selected(&r->providers[i], provider)) {
        chosen[count++] = &r->providers[i];
        ai_token_stats_add(&total, &r->providers[i].totals);
        sessions += r->providers[i].sessions_count;
    }
    if (view == 0) {
        char tokens[32]; ai_fmt_tokens(ai_token_stats_total(&total), tokens, sizeof(tokens));
        style(HEADING, A_BOLD);
        line(c, "%s TOKENS     %lld TURNS     %d SESSIONS", tokens, (long long)total.turns, sessions);
        style(MUTED, A_NORMAL);
        line(c, "Quota fill shows usage; vertical marker shows elapsed time. Projection estimates pace.");
        line(c, "");
        int columns = c->width >= 120 && count > 1 ? count : 1;
        int width = (c->width - 4 - (columns - 1) * 2) / columns;
        for (int i = 0; i < count;) {
            int n = count - i < columns ? count - i : columns;
            int card_height = 10;
            for (int j = 0; j < n; ++j) {
                int h = 7 + chosen[i + j]->n_quotas * 6;
                if (h > card_height) card_height = h;
            }
            for (int j = 0; j < n; ++j)
                quota_card(c, chosen[i + j], c->line, 2 + j * (width + 2), width, card_height, time(NULL));
            c->line += card_height + 1; i += n;
        }
        style(HEADING, A_BOLD); line(c, "USAGE BREAKDOWN");
        stats_header(c, "Sessions");
    } else if (view == 1 || view == 2) stats_header(c, view == 1 ? "Model" : "Date");
    else {
        style(HEADING, A_BOLD);
        line(c, "%-16s %-12s %9s %7s %-24s %s", "Updated", "Provider", "Total", "Turns", "Model", "Session / CWD");
        line(c, "");
    }
    memset(&total, 0, sizeof(total));
    for (int i = 0; i < count; ++i) {
        const ai_provider_usage_t *p = chosen[i];
        if (view == 0) {
            char label[32]; snprintf(label, sizeof(label), "%d", p->sessions_count);
            stats_line(c, p->provider, label, &p->totals);
            ai_token_stats_add(&total, &p->totals); ++rows;
        } else if (view == 1 || view == 2) {
            const ai_keyed_stats_t *items = view == 1 ? p->by_model : p->by_day;
            int n = view == 1 ? p->n_models : p->n_days;
            for (int j = 0; j < n; ++j) {
                stats_line(c, p->provider, items[j].key, &items[j].stats);
                ai_token_stats_add(&total, &items[j].stats); ++rows;
            }
        } else for (int j = 0; j < p->n_sessions; ++j) {
            const ai_session_info_t *s = &p->sessions[j];
            char timestamp[32] = "unknown", tokens[32], value[32];
            time_t epoch = (time_t)s->updated_epoch; struct tm tm;
            if (localtime_r(&epoch, &tm)) strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", &tm);
            ai_fmt_tokens(s->total_tokens, tokens, sizeof(tokens));
            int row = c->line++;
            style(MUTED, A_NORMAL); cell(c, row, 2, 16, timestamp);
            style(provider_color(p->provider), A_NORMAL); cell(c, row, 19, 12, p->provider);
            style(HEADING, A_NORMAL); snprintf(value, sizeof(value), "%9s", tokens); cell(c, row, 32, 9, value);
            snprintf(value, sizeof(value), "%7lld", (long long)s->turns); cell(c, row, 42, 7, value);
            style(MUTED, A_NORMAL);
            int model_width = c->width >= 120 ? 24 : 16;
            cell(c, row, 50, model_width, s->model);
            cell(c, row, 51 + model_width, c->width - 53 - model_width,
                 s->title[0] ? s->title : s->cwd[0] ? s->cwd : s->session_id);
            ++rows;
        }
    }
    style(MUTED, A_NORMAL);
    if (!rows) line(c, "No usage recorded for this provider and time range.");
    else if (view != 3) { line(c, ""); stats_line(c, "TOTAL", "", &total); }
    return c->line;
}

static bool number(const char *text, int minimum, int *out) {
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !*text || *end || value < minimum || value > INT_MAX) return false;
    *out = (int)value;
    return true;
}

static void usage(FILE *out) {
    fprintf(out, "Usage: ai-usage-tui [--days N | --all] [--provider all|claude|codex|antigravity|opencode]\n"
                 "                    [--refresh SECONDS] [--help]\n"
                 "Defaults: 7 days, all providers, refresh every 60 seconds (0 disables).\n"
                 "Keys: 1-4/Tab views, j/k/arrows scroll, PgUp/PgDn/Home/End,\n"
                 "      p provider, d days (1/7/30/all), r refresh, q quit.\n");
}

int main(int argc, char **argv) {
    int days = 7, provider = 0, refresh_seconds = 60;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) { usage(stdout); return 0; }
        if (!strcmp(arg, "--all")) { days = 0; continue; }
        if ((!strcmp(arg, "--days") || !strcmp(arg, "--refresh") || !strcmp(arg, "--provider")) && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(arg, "--provider")) {
                int found = -1;
                for (int p = 0; p < N_PROVIDERS; ++p) if (!strcmp(value, providers[p])) found = p;
                if (found >= 0) { provider = found; continue; }
            } else if (number(value, !strcmp(arg, "--days") ? 1 : 0,
                              !strcmp(arg, "--days") ? &days : &refresh_seconds)) continue;
            fprintf(stderr, "Invalid value for %s: %s\n", arg, value);
        } else fprintf(stderr, "Unknown option or missing value: %s\n", arg);
        usage(stderr);
        return 2;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "ai-usage-tui requires an interactive terminal. Use ai-usage or ai-usage --json for redirected output.\n");
        return 1;
    }
    if (!getenv("TERM") || !strcmp(getenv("TERM"), "dumb")) {
        fprintf(stderr, "ai-usage-tui requires a supported TERM setting.\n");
        return 1;
    }
    setlocale(LC_ALL, "");
    unicode_ui = !strcmp(nl_langinfo(CODESET), "UTF-8") || !strcmp(nl_langinfo(CODESET), "UTF8");
    collector_t collector = {.mutex = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER};
    pthread_t thread;
    int error = pthread_create(&thread, NULL, collect_worker, &collector);
    if (error) { fprintf(stderr, "Cannot start collector: %s\n", strerror(error)); return 1; }
    struct sigaction action = {0}, old_int, old_term;
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &old_int);
    sigaction(SIGTERM, &action, &old_term);
    if (!initscr()) {
        fprintf(stderr, "Cannot initialize terminal.\n");
        pthread_mutex_lock(&collector.mutex);
        collector.stop = true;
        pthread_cond_signal(&collector.wake);
        pthread_mutex_unlock(&collector.mutex);
        pthread_join(thread, NULL);
        return 1;
    }
    color_ui = has_colors();
    if (color_ui) init_theme();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);
    ai_usage_report_t report = {0};
    int view = 0, offset = 0;
    bool quit = false;
    time_t last_request = time(NULL);
    request_collect(&collector, days);
    while (!quit && !interrupted) {
        pthread_mutex_lock(&collector.mutex);
        if (collector.ready) {
            ai_usage_report_free(&report);
            report = collector.result;
            memset(&collector.result, 0, sizeof(collector.result));
            collector.ready = false;
        }
        bool busy = collector.busy || collector.requested;
        pthread_mutex_unlock(&collector.mutex);
        time_t now = time(NULL);
        if (!busy && refresh_seconds && difftime(now, last_request) >= refresh_seconds) {
            request_collect(&collector, days);
            last_request = now;
            busy = true;
        }
        int height, width;
        getmaxyx(stdscr, height, width);
        erase();
        if (height < 7 || width < 20) {
            safe_print(0, width, "Resize terminal (20x7 min)");
        } else {
            char text[512], range[32], timestamp[32] = "pending";
            if (days) snprintf(range, sizeof(range), "%d days", days);
            else snprintf(range, sizeof(range), "all time");
            time_t generated = (time_t)report.generated_at;
            struct tm tm;
            if (report.generated_at && localtime_r(&generated, &tm)) strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tm);
            style(HEADING, A_BOLD);
            safe_print(0, width, "  AI USAGE  /  Your coding activity");
            int tab_x = 2;
            for (int i = 0; i < 4; ++i) {
                snprintf(text, sizeof(text), " %d %s ", i + 1, views[i]);
                style(i == view ? TAB : MUTED, i == view && !color_ui ? A_UNDERLINE : A_NORMAL);
                put_text(1, tab_x, width - tab_x - 1, text);
                tab_x += (int)strlen(text) + 1;
            }
            style(MUTED, A_NORMAL);
            snprintf(text, sizeof(text), "  Provider: %s   Range: %s   Updated: %s%s%s", providers[provider], range,
                     timestamp, busy ? "   Refreshing..." : "", report.generated_at && report.days != days ? " (showing previous range)" : "");
            safe_print(2, width, text);
            canvas_t canvas = {.offset = offset, .height = height - 5, .width = width};
            int count = render_body(&canvas, &report, view, provider);
            int max_offset = count > canvas.height ? count - canvas.height : 0;
            if (offset > max_offset) { offset = max_offset; continue; }
            snprintf(text, sizeof(text), "p provider  d days  r refresh  j/k scroll  PgUp/PgDn Home/End  q quit | rows %d-%d/%d",
                     count ? offset + 1 : 0, count < offset + canvas.height ? count : offset + canvas.height, count);
            style(MUTED, A_DIM);
            safe_print(height - 1, width, text);
            style(MUTED, A_NORMAL);
        }
        refresh();
        int key = getch();
        int page = height > 5 ? height - 5 : 1;
        if (key == 'q') quit = true;
        else if (key >= '1' && key <= '4') { view = key - '1'; offset = 0; }
        else if (key == '\t') { view = (view + 1) % 4; offset = 0; }
        else if (key == 'p') { provider = (provider + 1) % N_PROVIDERS; offset = 0; }
        else if (key == 'd') {
            days = days == 1 ? 7 : days == 7 ? 30 : days == 30 ? 0 : 1;
            offset = 0;
            request_collect(&collector, days);
            last_request = now;
        } else if (key == 'r') { request_collect(&collector, days); last_request = now; }
        else if (key == 'j' || key == KEY_DOWN) { if (offset < INT_MAX) ++offset; }
        else if (key == 'k' || key == KEY_UP) { if (offset) --offset; }
        else if (key == KEY_NPAGE) { if (offset < INT_MAX - page) offset += page; }
        else if (key == KEY_PPAGE) offset = offset > page ? offset - page : 0;
        else if (key == KEY_HOME) offset = 0;
        else if (key == KEY_END) offset = INT_MAX;
    }
    if (color_ui) restore_theme();
    endwin();
    pthread_mutex_lock(&collector.mutex);
    collector.stop = true;
    pthread_cond_signal(&collector.wake);
    bool busy = collector.busy;
    pthread_mutex_unlock(&collector.mutex);
    if (busy) fprintf(stderr, "Waiting for the current collection to finish...\n");
    pthread_join(thread, NULL);
    if (collector.ready) ai_usage_report_free(&collector.result);
    ai_usage_report_free(&report);
    pthread_cond_destroy(&collector.wake);
    pthread_mutex_destroy(&collector.mutex);
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    return 0;
}
