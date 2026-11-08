#include "ai_usage/core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void ascii_bar(double used_pct, bool has_el, double elapsed_pct, int width, char *out, size_t cap) {
    if (cap < (size_t)(width + 3)) return;
    int fill = (int)lround((used_pct / 100.0) * (double)width);
    if (fill < 0) fill = 0;
    if (fill > width) fill = width;
    out[0] = '[';
    for (int i = 0; i < width; ++i) {
        out[1 + i] = (i < fill) ? '#' : '.';
    }
    if (has_el) {
        int mark = (int)((elapsed_pct / 100.0) * (double)width);
        if (mark < 0) mark = 0;
        if (mark >= width) mark = width - 1;
        out[1 + mark] = '|';
    }
    out[1 + width] = ']';
    out[2 + width] = '\0';
}

static void print_table(
    int n_cols,
    const char **headers,
    const bool *right_align,
    int n_rows,
    char *(*cells)[9]
) {
    size_t widths[9] = {0};
    for (int c = 0; c < n_cols; ++c) {
        widths[c] = strlen(headers[c]);
    }
    for (int r = 0; r < n_rows; ++r) {
        for (int c = 0; c < n_cols; ++c) {
            size_t len = strlen(cells[r][c] ? cells[r][c] : "");
            if (len > widths[c]) widths[c] = len;
        }
    }

    for (int c = 0; c < n_cols; ++c) {
        if (c > 0) printf("  ");
        if (right_align[c]) printf("%*s", (int)widths[c], headers[c]);
        else printf("%-*s", (int)widths[c], headers[c]);
    }
    printf("\n");

    for (int c = 0; c < n_cols; ++c) {
        if (c > 0) printf("  ");
        for (size_t k = 0; k < widths[c]; ++k) putchar('-');
    }
    printf("\n");

    for (int r = 0; r < n_rows; ++r) {
        for (int c = 0; c < n_cols; ++c) {
            const char *val = cells[r][c] ? cells[r][c] : "";
            if (c > 0) printf("  ");
            if (right_align[c]) printf("%*s", (int)widths[c], val);
            else printf("%-*s", (int)widths[c], val);
        }
        printf("\n");
    }
}

static void render_report(
    const ai_usage_report_t *rep,
    bool show_models,
    bool show_daily,
    bool show_sessions
) {
    double now = rep->generated_at;
    char win_str[32];
    if (rep->days > 0) snprintf(win_str, sizeof(win_str), "last %dd", rep->days);
    else snprintf(win_str, sizeof(win_str), "all time");

    time_t sec = (time_t)now;
    struct tm tm_val;
    localtime_r(&sec, &tm_val);
    char ts_str[32];
    strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M:%S", &tm_val);

    printf("=== AI Usage Report (%s, as of %s) ===\n\n", win_str, ts_str);

    printf("-- Quota / Rate Limits --\n");
    const char *q_hdr[6] = {
        "Provider", "Win", "Usage (| = time elapsed)", "Resets In", "Proj", "Source"
    };
    const bool q_align[6] = {false, false, false, true, true, false};
    char q_buf[12][6][64];
    char *q_cells[12][9];
    int n_qrows = 0;

    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        if (pu->n_quotas == 0) {
            snprintf(q_buf[n_qrows][0], 64, "%s", pu->provider);
            snprintf(q_buf[n_qrows][1], 64, "-");
            snprintf(q_buf[n_qrows][2], 64, "-");
            snprintf(q_buf[n_qrows][3], 64, "-");
            snprintf(q_buf[n_qrows][4], 64, "-");
            snprintf(q_buf[n_qrows][5], 64, "%s", pu->quota_note[0] ? pu->quota_note : "-");
            for (int c = 0; c < 6; ++c) q_cells[n_qrows][c] = q_buf[n_qrows][c];
            ++n_qrows;
            continue;
        }
        for (int q = 0; q < pu->n_quotas && n_qrows < 12; ++q) {
            const ai_quota_window_t *qw = &pu->quotas[q];
            double el = 0.0, proj = 0.0;
            bool has_el = ai_quota_elapsed_pct(qw, now, &el);
            bool has_proj = ai_quota_projected_pct(qw, now, &proj);
            char bar[32];
            ascii_bar(qw->used_pct, has_el, el, 18, bar, sizeof(bar));

            snprintf(q_buf[n_qrows][0], 64, "%s", pu->provider);
            snprintf(q_buf[n_qrows][1], 64, "%s", qw->label);
            snprintf(q_buf[n_qrows][2], 64, "%s %5.1f%%", bar, qw->used_pct);
            if (qw->has_reset) ai_fmt_duration(qw->reset_epoch - now, q_buf[n_qrows][3], 64);
            else snprintf(q_buf[n_qrows][3], 64, "-");
            if (has_proj) snprintf(q_buf[n_qrows][4], 64, "~%.0f%%", proj);
            else snprintf(q_buf[n_qrows][4], 64, "-");
            snprintf(q_buf[n_qrows][5], 64, "%s", pu->quota_note);
            for (int c = 0; c < 6; ++c) q_cells[n_qrows][c] = q_buf[n_qrows][c];
            ++n_qrows;
        }
    }
    print_table(6, q_hdr, q_align, n_qrows, q_cells);

    printf("\n-- Token Usage (%s) --\n", win_str);
    const char *p_hdr[9] = {
        "Provider", "Sess", "Turns", "Input", "CacheRead", "CacheWrite", "Output", "Reasoning", "Total"
    };
    const bool p_align[9] = {false, true, true, true, true, true, true, true, true};
    char p_buf[5][9][32];
    char *p_cells[5][9];
    int n_prows = 0;
    ai_token_stats_t grand = {0};
    int total_sess = 0;

    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        const ai_token_stats_t *t = &pu->totals;
        ai_token_stats_add(&grand, t);
        total_sess += pu->sessions_count;
        snprintf(p_buf[n_prows][0], 32, "%s", pu->provider);
        snprintf(p_buf[n_prows][1], 32, "%d", pu->sessions_count);
        snprintf(p_buf[n_prows][2], 32, "%lld", (long long)t->turns);
        ai_fmt_tokens(t->input_tokens, p_buf[n_prows][3], 32);
        ai_fmt_tokens(t->cache_read_tokens, p_buf[n_prows][4], 32);
        ai_fmt_tokens(t->cache_write_tokens, p_buf[n_prows][5], 32);
        ai_fmt_tokens(t->output_tokens, p_buf[n_prows][6], 32);
        ai_fmt_tokens(t->reasoning_tokens, p_buf[n_prows][7], 32);
        ai_fmt_tokens(ai_token_stats_total(t), p_buf[n_prows][8], 32);
        for (int c = 0; c < 9; ++c) p_cells[n_prows][c] = p_buf[n_prows][c];
        ++n_prows;
    }
    if (rep->n_providers > 1) {
        snprintf(p_buf[n_prows][0], 32, "TOTAL");
        snprintf(p_buf[n_prows][1], 32, "%d", total_sess);
        snprintf(p_buf[n_prows][2], 32, "%lld", (long long)grand.turns);
        ai_fmt_tokens(grand.input_tokens, p_buf[n_prows][3], 32);
        ai_fmt_tokens(grand.cache_read_tokens, p_buf[n_prows][4], 32);
        ai_fmt_tokens(grand.cache_write_tokens, p_buf[n_prows][5], 32);
        ai_fmt_tokens(grand.output_tokens, p_buf[n_prows][6], 32);
        ai_fmt_tokens(grand.reasoning_tokens, p_buf[n_prows][7], 32);
        ai_fmt_tokens(ai_token_stats_total(&grand), p_buf[n_prows][8], 32);
        for (int c = 0; c < 9; ++c) p_cells[n_prows][c] = p_buf[n_prows][c];
        ++n_prows;
    }
    print_table(9, p_hdr, p_align, n_prows, p_cells);

    if (show_models) {
        char m_buf[64][7][64];
        char *m_cells[64][9];
        int n_mrows = 0;
        for (int i = 0; i < rep->n_providers; ++i) {
            const ai_provider_usage_t *pu = &rep->providers[i];
            for (int m = 0; m < pu->n_models && n_mrows < 64; ++m) {
                const ai_token_stats_t *st = &pu->by_model[m].stats;
                snprintf(m_buf[n_mrows][0], 64, "%s", pu->provider);
                snprintf(m_buf[n_mrows][1], 64, "%.63s", pu->by_model[m].key);
                snprintf(m_buf[n_mrows][2], 64, "%lld", (long long)st->turns);
                ai_fmt_tokens(st->input_tokens, m_buf[n_mrows][3], 64);
                ai_fmt_tokens(st->cache_read_tokens, m_buf[n_mrows][4], 64);
                ai_fmt_tokens(st->output_tokens + st->reasoning_tokens, m_buf[n_mrows][5], 64);
                ai_fmt_tokens(ai_token_stats_total(st), m_buf[n_mrows][6], 64);
                for (int c = 0; c < 7; ++c) m_cells[n_mrows][c] = m_buf[n_mrows][c];
                ++n_mrows;
            }
        }
        if (n_mrows > 0) {
            printf("\n-- By Model --\n");
            const char *m_hdr[7] = {"Provider", "Model", "Turns", "Input", "CacheRead", "Out+Reas", "Total"};
            const bool m_align[7] = {false, false, true, true, true, true, true};
            print_table(7, m_hdr, m_align, n_mrows, m_cells);
        }
    }

    if (show_daily) {
        char d_buf[64][7][64];
        char *d_cells[64][9];
        int n_drows = 0;
        for (int i = 0; i < rep->n_providers; ++i) {
            const ai_provider_usage_t *pu = &rep->providers[i];
            for (int d = 0; d < pu->n_days && n_drows < 64; ++d) {
                const ai_token_stats_t *st = &pu->by_day[d].stats;
                snprintf(d_buf[n_drows][0], 64, "%.63s", pu->by_day[d].key);
                snprintf(d_buf[n_drows][1], 64, "%s", pu->provider);
                snprintf(d_buf[n_drows][2], 64, "%lld", (long long)st->turns);
                ai_fmt_tokens(st->input_tokens, d_buf[n_drows][3], 64);
                ai_fmt_tokens(st->cache_read_tokens, d_buf[n_drows][4], 64);
                ai_fmt_tokens(st->output_tokens + st->reasoning_tokens, d_buf[n_drows][5], 64);
                ai_fmt_tokens(ai_token_stats_total(st), d_buf[n_drows][6], 64);
                for (int c = 0; c < 7; ++c) d_cells[n_drows][c] = d_buf[n_drows][c];
                ++n_drows;
            }
        }
        if (n_drows > 0) {
            printf("\n-- Daily Breakdown --\n");
            const char *d_hdr[7] = {"Date", "Provider", "Turns", "Input", "CacheRead", "Out+Reas", "Total"};
            const bool d_align[7] = {false, false, true, true, true, true, true};
            print_table(7, d_hdr, d_align, n_drows, d_cells);
        }
    }

    if (show_sessions) {
        char s_buf[45][6][64];
        char *s_cells[45][9];
        int n_srows = 0;
        for (int i = 0; i < rep->n_providers; ++i) {
            const ai_provider_usage_t *pu = &rep->providers[i];
            int lim = pu->n_sessions < 15 ? pu->n_sessions : 15;
            for (int s = 0; s < lim && n_srows < 45; ++s) {
                const ai_session_info_t *si = &pu->sessions[s];
                time_t s_sec = (time_t)si->updated_epoch;
                struct tm s_tm;
                localtime_r(&s_sec, &s_tm);
                strftime(s_buf[n_srows][0], 64, "%m-%d %H:%M", &s_tm);
                snprintf(s_buf[n_srows][1], 64, "%s", si->provider);
                snprintf(s_buf[n_srows][2], 64, "%.22s", si->model);
                snprintf(s_buf[n_srows][3], 64, "%lld", (long long)si->turns);
                ai_fmt_tokens(si->total_tokens, s_buf[n_srows][4], 64);
                const char *loc = si->title[0] ? si->title : si->cwd;
                if (!loc || !*loc) loc = si->session_id;
                snprintf(s_buf[n_srows][5], 64, "%.36s", loc);
                for (int c = 0; c < 6; ++c) s_cells[n_srows][c] = s_buf[n_srows][c];
                ++n_srows;
            }
        }
        if (n_srows > 0) {
            printf("\n-- Recent Sessions --\n");
            const char *s_hdr[6] = {"Updated", "Provider", "Model", "Turns", "Total", "Session / CWD"};
            const bool s_align[6] = {false, false, false, true, true, false};
            print_table(6, s_hdr, s_align, n_srows, s_cells);
        }
    }
}

int main(int argc, char **argv) {
    int days = 7;
    bool all = false;
    bool show_models = false;
    bool show_daily = false;
    bool show_sessions = false;
    bool emit_json = false;
    int mask = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--days") == 0 && i + 1 < argc) {
            days = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--all") == 0) {
            all = true;
        } else if (strcmp(argv[i], "--models") == 0) {
            show_models = true;
        } else if (strcmp(argv[i], "--daily") == 0) {
            show_daily = true;
        } else if (strcmp(argv[i], "--sessions") == 0) {
            show_sessions = true;
        } else if (strcmp(argv[i], "--json") == 0) {
            emit_json = true;
        } else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            if (strcmp(p, "claude") == 0) mask |= AI_PROVIDER_CLAUDE;
            else if (strcmp(p, "codex") == 0) mask |= AI_PROVIDER_CODEX;
            else if (strcmp(p, "antigravity") == 0) mask |= AI_PROVIDER_ANTIGRAVITY;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: ai-usage [--days N] [--all] [--provider claude|codex|antigravity] [--models] [--daily] [--sessions] [--json]\n");
            return 0;
        }
    }
    if (mask == 0) mask = AI_PROVIDER_ALL;
    int eff_days = (all || days <= 0) ? 0 : days;

    ai_usage_report_t rep;
    ai_usage_collect(&rep, eff_days, mask, 0.0, NULL, NULL, NULL);
    if (emit_json) {
        char *j = ai_usage_report_to_json(&rep);
        if (j) {
            puts(j);
            ai_usage_free_string(j);
        }
    } else {
        render_report(&rep, show_models || all, show_daily || all, show_sessions || all);
    }
    ai_usage_report_free(&rep);
    return 0;
}
