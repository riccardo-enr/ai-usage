#include "ai_usage/core.h"

#include <adwaita.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *APP_CSS =
    ".card-box {\n"
    "    background-color: alpha(@card_bg_color, 0.95);\n"
    "    border-radius: 12px;\n"
    "    padding: 14px 16px;\n"
    "    border: 1px solid alpha(@borders, 0.55);\n"
    "}\n"
    ".metric-title {\n"
    "    font-size: 11px;\n"
    "    font-weight: 600;\n"
    "    letter-spacing: 0.4px;\n"
    "    opacity: 0.68;\n"
    "}\n"
    ".metric-value {\n"
    "    font-family: monospace;\n"
    "    font-size: 20px;\n"
    "    font-weight: 700;\n"
    "}\n"
    ".metric-sub {\n"
    "    font-family: monospace;\n"
    "    font-size: 11.5px;\n"
    "    opacity: 0.75;\n"
    "}\n"
    ".mono-cell {\n"
    "    font-family: monospace;\n"
    "    font-size: 12.5px;\n"
    "}\n"
    ".provider-badge {\n"
    "    font-family: monospace;\n"
    "    font-size: 11px;\n"
    "    font-weight: 700;\n"
    "    padding: 2px 8px;\n"
    "    border-radius: 6px;\n"
    "    background-color: alpha(@accent_bg_color, 0.18);\n"
    "    color: @accent_color;\n"
    "}\n"
    ".section-header {\n"
    "    font-size: 14px;\n"
    "    font-weight: 700;\n"
    "    margin-top: 6px;\n"
    "    margin-bottom: 4px;\n"
    "}\n";

typedef struct {
    double used_pct;
    double elapsed_pct;
    bool has_elapsed;
} bar_data_t;

typedef struct {
    AdwApplicationWindow *win;
    GtkWidget *overview_box;
    GtkWidget *models_box;
    GtkWidget *daily_box;
    GtkWidget *sessions_box;
    GtkWidget *refresh_btn;
    GtkWidget *status_label;
    int selected_days;
    int selected_mask;
    bool loading;
} app_ui_t;

typedef struct {
    app_ui_t *ui;
    int days;
    int mask;
    ai_usage_report_t report;
    double elapsed_ms;
} worker_ctx_t;

static void draw_quota_bar(
    GtkDrawingArea *area,
    cairo_t *cr,
    int width,
    int height,
    gpointer user_data
) {
    (void)area;
    bar_data_t *bd = (bar_data_t *)user_data;
    double r = (double)height / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, width - r, r, r, -M_PI / 2.0, M_PI / 2.0);
    cairo_arc(cr, r, r, r, M_PI / 2.0, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.50, 0.54, 0.60, 0.22);
    cairo_fill(cr);

    double clamped = bd->used_pct;
    if (clamped < 0.0) clamped = 0.0;
    if (clamped > 100.0) clamped = 100.0;
    double fill_w = (clamped / 100.0) * (double)width;
    if (fill_w > 1.0) {
        cairo_save(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, width - r, r, r, -M_PI / 2.0, M_PI / 2.0);
        cairo_arc(cr, r, r, r, M_PI / 2.0, 3.0 * M_PI / 2.0);
        cairo_close_path(cr);
        cairo_clip(cr);

        if (clamped >= 90.0 ||
            (bd->has_elapsed && bd->elapsed_pct >= 5.0 && bd->used_pct > bd->elapsed_pct * 1.25)) {
            cairo_set_source_rgba(cr, 0.90, 0.32, 0.28, 0.92);
        } else if (clamped >= 75.0 ||
                   (bd->has_elapsed && bd->elapsed_pct >= 5.0 && bd->used_pct > bd->elapsed_pct * 1.05)) {
            cairo_set_source_rgba(cr, 0.92, 0.65, 0.20, 0.92);
        } else {
            cairo_set_source_rgba(cr, 0.24, 0.68, 0.44, 0.92);
        }
        cairo_rectangle(cr, 0, 0, fill_w < 2.0 ? 2.0 : fill_w, height);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    if (bd->has_elapsed && bd->elapsed_pct >= 0.0 && bd->elapsed_pct <= 100.0) {
        double mx = (bd->elapsed_pct / 100.0) * (double)width;
        if (mx < 2.0) mx = 2.0;
        if (mx > (double)width - 2.0) mx = (double)width - 2.0;
        cairo_set_source_rgba(cr, 0.96, 0.97, 0.99, 0.95);
        cairo_set_line_width(cr, 2.2);
        cairo_move_to(cr, mx, 0);
        cairo_line_to(cr, mx, height);
        cairo_stroke(cr);
    }
}

static GtkWidget *make_quota_drawing_area(double used_pct, bool has_elapsed, double elapsed_pct) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 16);
    gtk_widget_set_hexpand(area, TRUE);
    bar_data_t *bd = g_new0(bar_data_t, 1);
    bd->used_pct = used_pct;
    bd->has_elapsed = has_elapsed;
    bd->elapsed_pct = elapsed_pct;
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_quota_bar, bd, g_free);
    return area;
}

static void clear_box(GtkWidget *box) {
    GtkWidget *child = gtk_widget_get_first_child(box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

static GtkWidget *create_table_card(
    int n_cols,
    const char **headers,
    const bool *right_align,
    int n_rows,
    char *(*cells)[9]
) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "card-box");

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_hexpand(grid, TRUE);

    for (int c = 0; c < n_cols; ++c) {
        GtkWidget *lbl = gtk_label_new(headers[c]);
        gtk_label_set_xalign(GTK_LABEL(lbl), right_align[c] ? 1.0f : 0.0f);
        gtk_widget_add_css_class(lbl, "metric-title");
        if (c == n_cols - 1 && !right_align[c]) {
            gtk_widget_set_hexpand(lbl, TRUE);
        }
        gtk_grid_attach(GTK_GRID(grid), lbl, c, 0, 1, 1);
    }

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, 1, n_cols, 1);

    for (int r = 0; r < n_rows; ++r) {
        for (int c = 0; c < n_cols; ++c) {
            GtkWidget *lbl = gtk_label_new(cells[r][c] ? cells[r][c] : "");
            gtk_label_set_xalign(GTK_LABEL(lbl), right_align[c] ? 1.0f : 0.0f);
            gtk_widget_add_css_class(lbl, "mono-cell");
            gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            if (c == 1 || c == n_cols - 1) {
                gtk_widget_set_hexpand(lbl, TRUE);
            }
            gtk_grid_attach(GTK_GRID(grid), lbl, c, r + 2, 1, 1);
        }
    }

    gtk_box_append(GTK_BOX(card), grid);
    return card;
}

static void populate_overview(app_ui_t *ui, const ai_usage_report_t *rep) {
    clear_box(ui->overview_box);
    double now = rep->generated_at;

    GtkWidget *q_hdr = gtk_label_new("Quota & Rate Limits (| = elapsed window marker)");
    gtk_label_set_xalign(GTK_LABEL(q_hdr), 0.0f);
    gtk_widget_add_css_class(q_hdr, "section-header");
    gtk_box_append(GTK_BOX(ui->overview_box), q_hdr);

    GtkWidget *q_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(card, "card-box");

        GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        char up_prov[32];
        size_t plen = strlen(pu->provider);
        for (size_t k = 0; k < plen && k + 1 < sizeof(up_prov); ++k) {
            up_prov[k] = (char)g_ascii_toupper(pu->provider[k]);
            up_prov[k + 1] = '\0';
        }
        GtkWidget *badge = gtk_label_new(up_prov);
        gtk_widget_add_css_class(badge, "provider-badge");
        gtk_box_append(GTK_BOX(top_row), badge);

        char src_txt[128];
        snprintf(src_txt, sizeof(src_txt), "source: %s", pu->quota_note[0] ? pu->quota_note : "-");
        GtkWidget *src_lbl = gtk_label_new(src_txt);
        gtk_label_set_xalign(GTK_LABEL(src_lbl), 1.0f);
        gtk_widget_set_hexpand(src_lbl, TRUE);
        gtk_widget_add_css_class(src_lbl, "metric-sub");
        gtk_box_append(GTK_BOX(top_row), src_lbl);
        gtk_box_append(GTK_BOX(card), top_row);

        if (pu->n_quotas == 0) {
            GtkWidget *empty_lbl = gtk_label_new("No live quota window reported (transcript token counts below)");
            gtk_label_set_xalign(GTK_LABEL(empty_lbl), 0.0f);
            gtk_widget_add_css_class(empty_lbl, "metric-sub");
            gtk_box_append(GTK_BOX(card), empty_lbl);
        } else {
            for (int q = 0; q < pu->n_quotas; ++q) {
                const ai_quota_window_t *qw = &pu->quotas[q];
                double el = 0.0, proj = 0.0;
                bool has_el = ai_quota_elapsed_pct(qw, now, &el);
                bool has_proj = ai_quota_projected_pct(qw, now, &proj);

                char rem_s[32] = "-";
                if (qw->has_reset) {
                    ai_fmt_duration(qw->reset_epoch - now, rem_s, sizeof(rem_s));
                }
                char el_s[32], proj_s[32];
                if (has_el) snprintf(el_s, sizeof(el_s), "%.0f%% elapsed", el);
                else snprintf(el_s, sizeof(el_s), "elapsed n/a");
                if (has_proj) snprintf(proj_s, sizeof(proj_s), "proj ~%.0f%%", proj);
                else snprintf(proj_s, sizeof(proj_s), "proj n/a");

                GtkWidget *w_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
                char w_lbl_txt[16];
                snprintf(w_lbl_txt, sizeof(w_lbl_txt), "%3s", qw->label);
                GtkWidget *w_lbl = gtk_label_new(w_lbl_txt);
                gtk_widget_add_css_class(w_lbl, "mono-cell");
                gtk_widget_set_size_request(w_lbl, 36, -1);
                gtk_box_append(GTK_BOX(w_row), w_lbl);

                GtkWidget *bar = make_quota_drawing_area(qw->used_pct, has_el, el);
                gtk_box_append(GTK_BOX(w_row), bar);

                char meta_txt[160];
                snprintf(
                    meta_txt,
                    sizeof(meta_txt),
                    "%5.1f%%  |  resets in %7s  |  %11s  |  %10s",
                    qw->used_pct,
                    rem_s,
                    el_s,
                    proj_s
                );
                GtkWidget *meta_lbl = gtk_label_new(meta_txt);
                gtk_label_set_xalign(GTK_LABEL(meta_lbl), 1.0f);
                gtk_widget_add_css_class(meta_lbl, "metric-sub");
                gtk_box_append(GTK_BOX(w_row), meta_lbl);

                gtk_box_append(GTK_BOX(card), w_row);
            }
        }
        gtk_box_append(GTK_BOX(q_list), card);
    }
    gtk_box_append(GTK_BOX(ui->overview_box), q_list);

    char win_hdr_txt[96];
    if (rep->days > 0) {
        snprintf(win_hdr_txt, sizeof(win_hdr_txt), "Token Usage Summary (last %dd)", rep->days);
    } else {
        snprintf(win_hdr_txt, sizeof(win_hdr_txt), "Token Usage Summary (all time)");
    }
    GtkWidget *t_hdr = gtk_label_new(win_hdr_txt);
    gtk_label_set_xalign(GTK_LABEL(t_hdr), 0.0f);
    gtk_widget_add_css_class(t_hdr, "section-header");
    gtk_box_append(GTK_BOX(ui->overview_box), t_hdr);

    ai_token_stats_t grand = {0};
    int total_sess = 0;
    for (int i = 0; i < rep->n_providers; ++i) {
        ai_token_stats_add(&grand, &rep->providers[i].totals);
        total_sess += rep->providers[i].sessions_count;
    }

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 10);

    char v0[32], v1[32], v2[32], v3[32], cw_s[32], out_s[32], reas_s[32];
    ai_fmt_tokens(ai_token_stats_total(&grand), v0, sizeof(v0));
    ai_fmt_tokens(grand.input_tokens, v1, sizeof(v1));
    ai_fmt_tokens(grand.cache_read_tokens, v2, sizeof(v2));
    ai_fmt_tokens(grand.output_tokens + grand.reasoning_tokens, v3, sizeof(v3));
    ai_fmt_tokens(grand.cache_write_tokens, cw_s, sizeof(cw_s));
    ai_fmt_tokens(grand.output_tokens, out_s, sizeof(out_s));
    ai_fmt_tokens(grand.reasoning_tokens, reas_s, sizeof(reas_s));

    char s0[96], s1[96], s2[96], s3[96];
    snprintf(s0, sizeof(s0), "%d sessions, %lld turns", total_sess, (long long)grand.turns);
    snprintf(s1, sizeof(s1), "cache write: %s", cw_s);
    int64_t denom = grand.input_tokens + grand.cache_read_tokens + grand.cache_write_tokens;
    double hit_pct = denom > 0 ? (100.0 * (double)grand.cache_read_tokens / (double)denom) : 0.0;
    snprintf(s2, sizeof(s2), "cache hit: %.1f%%", hit_pct);
    snprintf(s3, sizeof(s3), "out: %s, reas: %s", out_s, reas_s);

    const char *m_titles[4] = {"TOTAL TOKENS", "INPUT (UNCACHED)", "CACHE READ", "OUTPUT + REASONING"};
    const char *m_vals[4] = {v0, v1, v2, v3};
    const char *m_subs[4] = {s0, s1, s2, s3};
    for (int k = 0; k < 4; ++k) {
        GtkWidget *cbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_add_css_class(cbox, "card-box");
        GtkWidget *tl = gtk_label_new(m_titles[k]);
        gtk_label_set_xalign(GTK_LABEL(tl), 0.0f);
        gtk_widget_add_css_class(tl, "metric-title");
        GtkWidget *vl = gtk_label_new(m_vals[k]);
        gtk_label_set_xalign(GTK_LABEL(vl), 0.0f);
        gtk_widget_add_css_class(vl, "metric-value");
        GtkWidget *sl = gtk_label_new(m_subs[k]);
        gtk_label_set_xalign(GTK_LABEL(sl), 0.0f);
        gtk_widget_add_css_class(sl, "metric-sub");
        gtk_box_append(GTK_BOX(cbox), tl);
        gtk_box_append(GTK_BOX(cbox), vl);
        gtk_box_append(GTK_BOX(cbox), sl);
        gtk_flow_box_append(GTK_FLOW_BOX(flow), cbox);
    }
    gtk_box_append(GTK_BOX(ui->overview_box), flow);

    const char *headers[9] = {
        "Provider", "Sess", "Turns", "Input", "CacheRead", "CacheWrite", "Output", "Reas", "Total"
    };
    const bool ralign[9] = {false, true, true, true, true, true, true, true, true};
    char buf[4][9][32];
    char *cells[4][9];
    int n_rows = 0;

    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        const ai_token_stats_t *t = &pu->totals;
        snprintf(buf[n_rows][0], 32, "%s", pu->provider);
        snprintf(buf[n_rows][1], 32, "%d", pu->sessions_count);
        snprintf(buf[n_rows][2], 32, "%lld", (long long)t->turns);
        ai_fmt_tokens(t->input_tokens, buf[n_rows][3], 32);
        ai_fmt_tokens(t->cache_read_tokens, buf[n_rows][4], 32);
        ai_fmt_tokens(t->cache_write_tokens, buf[n_rows][5], 32);
        ai_fmt_tokens(t->output_tokens, buf[n_rows][6], 32);
        ai_fmt_tokens(t->reasoning_tokens, buf[n_rows][7], 32);
        ai_fmt_tokens(ai_token_stats_total(t), buf[n_rows][8], 32);
        for (int c = 0; c < 9; ++c) cells[n_rows][c] = buf[n_rows][c];
        ++n_rows;
    }
    if (rep->n_providers > 1) {
        snprintf(buf[n_rows][0], 32, "TOTAL");
        snprintf(buf[n_rows][1], 32, "%d", total_sess);
        snprintf(buf[n_rows][2], 32, "%lld", (long long)grand.turns);
        ai_fmt_tokens(grand.input_tokens, buf[n_rows][3], 32);
        ai_fmt_tokens(grand.cache_read_tokens, buf[n_rows][4], 32);
        ai_fmt_tokens(grand.cache_write_tokens, buf[n_rows][5], 32);
        ai_fmt_tokens(grand.output_tokens, buf[n_rows][6], 32);
        ai_fmt_tokens(grand.reasoning_tokens, buf[n_rows][7], 32);
        ai_fmt_tokens(ai_token_stats_total(&grand), buf[n_rows][8], 32);
        for (int c = 0; c < 9; ++c) cells[n_rows][c] = buf[n_rows][c];
        ++n_rows;
    }

    gtk_box_append(
        GTK_BOX(ui->overview_box),
        create_table_card(9, headers, ralign, n_rows, cells)
    );
}

static void populate_models(app_ui_t *ui, const ai_usage_report_t *rep) {
    clear_box(ui->models_box);
    GtkWidget *hdr = gtk_label_new("Usage by Model");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
    gtk_widget_add_css_class(hdr, "section-header");
    gtk_box_append(GTK_BOX(ui->models_box), hdr);

    const char *headers[7] = {"Provider", "Model", "Turns", "Input", "CacheRead", "Out+Reas", "Total"};
    const bool ralign[7] = {false, false, true, true, true, true, true};
    char buf[64][7][64];
    char *cells[64][9];
    int n_rows = 0;

    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        for (int m = 0; m < pu->n_models && n_rows < 64; ++m) {
            const ai_token_stats_t *st = &pu->by_model[m].stats;
            snprintf(buf[n_rows][0], 64, "%s", pu->provider);
            snprintf(buf[n_rows][1], 64, "%.63s", pu->by_model[m].key);
            snprintf(buf[n_rows][2], 64, "%lld", (long long)st->turns);
            ai_fmt_tokens(st->input_tokens, buf[n_rows][3], 64);
            ai_fmt_tokens(st->cache_read_tokens, buf[n_rows][4], 64);
            ai_fmt_tokens(st->output_tokens + st->reasoning_tokens, buf[n_rows][5], 64);
            ai_fmt_tokens(ai_token_stats_total(st), buf[n_rows][6], 64);
            for (int c = 0; c < 7; ++c) cells[n_rows][c] = buf[n_rows][c];
            ++n_rows;
        }
    }
    if (n_rows == 0) {
        const char *empty[7] = {"-", "No model usage in window", "0", "0", "0", "0", "0"};
        for (int c = 0; c < 7; ++c) {
            snprintf(buf[0][c], 64, "%s", empty[c]);
            cells[0][c] = buf[0][c];
        }
        n_rows = 1;
    }
    gtk_box_append(GTK_BOX(ui->models_box), create_table_card(7, headers, ralign, n_rows, cells));
}

typedef struct {
    char day[16];
    const char *provider;
    ai_token_stats_t stats;
} day_row_t;

static int cmp_day_row_desc(const void *a, const void *b) {
    const day_row_t *ra = (const day_row_t *)a;
    const day_row_t *rb = (const day_row_t *)b;
    int c = strcmp(rb->day, ra->day);
    if (c != 0) return c;
    return strcmp(ra->provider, rb->provider);
}

static void populate_daily(app_ui_t *ui, const ai_usage_report_t *rep) {
    clear_box(ui->daily_box);
    GtkWidget *hdr = gtk_label_new("Daily Usage Breakdown");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
    gtk_widget_add_css_class(hdr, "section-header");
    gtk_box_append(GTK_BOX(ui->daily_box), hdr);

    day_row_t drows[128];
    int n_drows = 0;
    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        for (int d = 0; d < pu->n_days && n_drows < 128; ++d) {
            snprintf(drows[n_drows].day, sizeof(drows[n_drows].day), "%.15s", pu->by_day[d].key);
            drows[n_drows].provider = pu->provider;
            drows[n_drows].stats = pu->by_day[d].stats;
            ++n_drows;
        }
    }
    if (n_drows > 1) qsort(drows, (size_t)n_drows, sizeof(day_row_t), cmp_day_row_desc);

    const char *headers[7] = {"Date", "Provider", "Turns", "Input", "CacheRead", "Out+Reas", "Total"};
    const bool ralign[7] = {false, false, true, true, true, true, true};
    char buf[64][7][64];
    char *cells[64][9];
    int n_rows = 0;

    for (int i = 0; i < n_drows && n_rows < 64; ++i) {
        const ai_token_stats_t *st = &drows[i].stats;
        snprintf(buf[n_rows][0], 64, "%s", drows[i].day);
        snprintf(buf[n_rows][1], 64, "%s", drows[i].provider);
        snprintf(buf[n_rows][2], 64, "%lld", (long long)st->turns);
        ai_fmt_tokens(st->input_tokens, buf[n_rows][3], 64);
        ai_fmt_tokens(st->cache_read_tokens, buf[n_rows][4], 64);
        ai_fmt_tokens(st->output_tokens + st->reasoning_tokens, buf[n_rows][5], 64);
        ai_fmt_tokens(ai_token_stats_total(st), buf[n_rows][6], 64);
        for (int c = 0; c < 7; ++c) cells[n_rows][c] = buf[n_rows][c];
        ++n_rows;
    }
    if (n_rows == 0) {
        const char *empty[7] = {"-", "-", "0", "0", "0", "0", "0"};
        for (int c = 0; c < 7; ++c) {
            snprintf(buf[0][c], 64, "%s", empty[c]);
            cells[0][c] = buf[0][c];
        }
        n_rows = 1;
    }
    gtk_box_append(GTK_BOX(ui->daily_box), create_table_card(7, headers, ralign, n_rows, cells));
}

static int cmp_session_ptr_desc(const void *a, const void *b) {
    const ai_session_info_t *sa = *(const ai_session_info_t *const *)a;
    const ai_session_info_t *sb = *(const ai_session_info_t *const *)b;
    if (sa->updated_epoch > sb->updated_epoch) return -1;
    if (sa->updated_epoch < sb->updated_epoch) return 1;
    return 0;
}

static void populate_sessions(app_ui_t *ui, const ai_usage_report_t *rep) {
    clear_box(ui->sessions_box);
    GtkWidget *hdr = gtk_label_new("Recent Sessions");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
    gtk_widget_add_css_class(hdr, "section-header");
    gtk_box_append(GTK_BOX(ui->sessions_box), hdr);

    const ai_session_info_t *ptrs[64];
    int n_ptrs = 0;
    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        int lim = pu->n_sessions < 15 ? pu->n_sessions : 15;
        for (int s = 0; s < lim && n_ptrs < 64; ++s) {
            ptrs[n_ptrs++] = &pu->sessions[s];
        }
    }
    if (n_ptrs > 1) qsort(ptrs, (size_t)n_ptrs, sizeof(ptrs[0]), cmp_session_ptr_desc);

    const char *headers[6] = {"Updated", "Provider", "Model", "Turns", "Total", "Session / CWD"};
    const bool ralign[6] = {false, false, false, true, true, false};
    char buf[30][6][96];
    char *cells[30][9];
    int n_rows = 0;

    for (int i = 0; i < n_ptrs && n_rows < 25; ++i) {
        const ai_session_info_t *s = ptrs[i];
        time_t sec = (time_t)s->updated_epoch;
        struct tm tm_val;
        localtime_r(&sec, &tm_val);
        strftime(buf[n_rows][0], 96, "%m-%d %H:%M", &tm_val);
        snprintf(buf[n_rows][1], 96, "%s", s->provider);
        snprintf(buf[n_rows][2], 96, "%.24s", s->model);
        snprintf(buf[n_rows][3], 96, "%lld", (long long)s->turns);
        ai_fmt_tokens(s->total_tokens, buf[n_rows][4], 96);

        const char *loc = s->title;
        if (!loc || !*loc) {
            const char *slash = strrchr(s->cwd, '/');
            loc = (slash && *(slash + 1)) ? (slash + 1) : s->cwd;
        }
        if (!loc || !*loc) loc = s->session_id;
        snprintf(buf[n_rows][5], 96, "%.48s", loc);

        for (int c = 0; c < 6; ++c) cells[n_rows][c] = buf[n_rows][c];
        ++n_rows;
    }
    if (n_rows == 0) {
        const char *empty[6] = {"-", "-", "-", "0", "0", "No sessions found"};
        for (int c = 0; c < 6; ++c) {
            snprintf(buf[0][c], 96, "%s", empty[c]);
            cells[0][c] = buf[0][c];
        }
        n_rows = 1;
    }
    gtk_box_append(GTK_BOX(ui->sessions_box), create_table_card(6, headers, ralign, n_rows, cells));
}

static gboolean apply_report_idle(gpointer data) {
    worker_ctx_t *ctx = (worker_ctx_t *)data;
    app_ui_t *ui = ctx->ui;

    ui->loading = false;
    gtk_widget_set_sensitive(ui->refresh_btn, TRUE);
    char status_txt[64];
    snprintf(status_txt, sizeof(status_txt), "C core (%.1f ms)", ctx->elapsed_ms);
    gtk_label_set_text(GTK_LABEL(ui->status_label), status_txt);

    populate_overview(ui, &ctx->report);
    populate_models(ui, &ctx->report);
    populate_daily(ui, &ctx->report);
    populate_sessions(ui, &ctx->report);

    ai_usage_report_free(&ctx->report);
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

static gpointer worker_thread_fn(gpointer data) {
    worker_ctx_t *ctx = (worker_ctx_t *)data;
    gint64 t0 = g_get_monotonic_time();
    ai_usage_collect(&ctx->report, ctx->days, ctx->mask, 0.0, NULL, NULL, NULL);
    ctx->elapsed_ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
    g_idle_add(apply_report_idle, ctx);
    return NULL;
}

static void refresh_async(app_ui_t *ui) {
    if (ui->loading) return;
    ui->loading = true;
    gtk_widget_set_sensitive(ui->refresh_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(ui->status_label), "Refreshing...");

    worker_ctx_t *ctx = g_new0(worker_ctx_t, 1);
    ctx->ui = ui;
    ctx->days = ui->selected_days;
    ctx->mask = ui->selected_mask;
    GThread *th = g_thread_new("ai-usage-worker", worker_thread_fn, ctx);
    g_thread_unref(th);
}

static void on_days_toggled(GtkToggleButton *btn, gpointer user_data) {
    if (!gtk_toggle_button_get_active(btn)) return;
    app_ui_t *ui = (app_ui_t *)user_data;
    int days = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "days-val"));
    ui->selected_days = days;
    refresh_async(ui);
}

static void on_provider_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    app_ui_t *ui = (app_ui_t *)user_data;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    const int masks[4] = {
        AI_PROVIDER_ALL,
        AI_PROVIDER_CLAUDE,
        AI_PROVIDER_CODEX,
        AI_PROVIDER_ANTIGRAVITY,
    };
    if (idx < 4) {
        ui->selected_mask = masks[idx];
        refresh_async(ui);
    }
}

static void on_refresh_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    refresh_async((app_ui_t *)user_data);
}

static gboolean on_auto_refresh(gpointer user_data) {
    refresh_async((app_ui_t *)user_data);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *make_scrolled_page(GtkWidget **out_inner) {
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC
    );
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 980);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 720);

    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(inner, 16);
    gtk_widget_set_margin_bottom(inner, 24);
    gtk_widget_set_margin_start(inner, 16);
    gtk_widget_set_margin_end(inner, 16);

    adw_clamp_set_child(ADW_CLAMP(clamp), inner);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);
    *out_inner = inner;
    return scrolled;
}

static void on_activate(AdwApplication *app, gpointer user_data) {
    (void)user_data;
    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, APP_CSS);
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
        g_object_unref(provider);
    }

    app_ui_t *ui = g_new0(app_ui_t, 1);
    ui->selected_days = 7;
    ui->selected_mask = AI_PROVIDER_ALL;

    ui->win = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    gtk_window_set_title(GTK_WINDOW(ui->win), "AI Usage");
    gtk_window_set_default_size(GTK_WINDOW(ui->win), 1040, 740);

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_application_window_set_content(ui->win, toolbar);

    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

    GtkWidget *stack = adw_view_stack_new();
    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(stack));
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);

    GtkWidget *days_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(days_box, "linked");
    const char *d_labels[4] = {"1d", "7d", "30d", "All"};
    const int d_vals[4] = {1, 7, 30, 0};
    GtkToggleButton *first_btn = NULL;
    for (int i = 0; i < 4; ++i) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(d_labels[i]);
        g_object_set_data(G_OBJECT(btn), "days-val", GINT_TO_POINTER(d_vals[i]));
        if (first_btn) {
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), first_btn);
        } else {
            first_btn = GTK_TOGGLE_BUTTON(btn);
        }
        if (d_vals[i] == ui->selected_days) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);
        }
        g_signal_connect(btn, "toggled", G_CALLBACK(on_days_toggled), ui);
        gtk_box_append(GTK_BOX(days_box), btn);
    }
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), days_box);

    const char *prov_items[5] = {"All Providers", "Claude", "Codex", "Antigravity", NULL};
    GtkWidget *dropdown = gtk_drop_down_new_from_strings(prov_items);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), 0);
    g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_provider_changed), ui);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), dropdown);

    ui->refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(ui->refresh_btn, "Refresh usage now");
    g_signal_connect(ui->refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), ui);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), ui->refresh_btn);

    ui->status_label = gtk_label_new("Ready");
    gtk_widget_add_css_class(ui->status_label, "metric-sub");
    gtk_widget_set_margin_end(ui->status_label, 8);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), ui->status_label);

    adw_view_stack_add_titled_with_icon(
        ADW_VIEW_STACK(stack),
        make_scrolled_page(&ui->overview_box),
        "overview",
        "Overview",
        "speedometer-symbolic"
    );
    adw_view_stack_add_titled_with_icon(
        ADW_VIEW_STACK(stack),
        make_scrolled_page(&ui->models_box),
        "models",
        "Models",
        "view-list-symbolic"
    );
    adw_view_stack_add_titled_with_icon(
        ADW_VIEW_STACK(stack),
        make_scrolled_page(&ui->daily_box),
        "daily",
        "Daily",
        "x-office-calendar-symbolic"
    );
    adw_view_stack_add_titled_with_icon(
        ADW_VIEW_STACK(stack),
        make_scrolled_page(&ui->sessions_box),
        "sessions",
        "Sessions",
        "utilities-terminal-symbolic"
    );

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), stack);

    g_timeout_add_seconds(30, on_auto_refresh, ui);
    refresh_async(ui);
    gtk_window_present(GTK_WINDOW(ui->win));
}

int main(int argc, char **argv) {
    AdwApplication *app = adw_application_new("com.riccardo.AIUsage", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
