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
    "    border-radius: 16px;\n"
    "    padding: 20px;\n"
    "    border: 1px solid alpha(@borders, 0.55);\n"
    "}\n"
    ".metric-title {\n"
    "    font-size: 12px;\n"
    "    font-weight: 600;\n"
    "    opacity: 0.68;\n"
    "}\n"
    ".metric-value {\n"
    "    font-size: 28px;\n"
    "    font-feature-settings: 'tnum';\n"
    "    font-weight: 700;\n"
    "}\n"
    ".metric-sub {\n"
    "    font-size: 12px;\n"
    "    opacity: 0.75;\n"
    "}\n"
    ".mono-cell {\n"
    "    font-family: monospace;\n"
    "    font-size: 12.5px;\n"
    "}\n"
    ".provider-title {\n"
    "    font-size: 17px;\n"
    "    font-weight: 700;\n"
    "}\n"
    ".provider-dot {\n"
    "    min-width: 9px;\n"
    "    min-height: 9px;\n"
    "    border-radius: 5px;\n"
    "}\n"
    ".provider-dot.claude { background-color: #e17a4d; }\n"
    ".provider-dot.codex { background-color: #429eeb; }\n"
    ".provider-dot.antigravity { background-color: #47c28a; }\n"
    ".filter-toolbar {\n"
    "    padding: 8px 20px 12px;\n"
    "}\n"
    ".section-header {\n"
    "    font-size: 18px;\n"
    "    font-weight: 700;\n"
    "    margin-top: 12px;\n"
    "    margin-bottom: 2px;\n"
    "}\n";

typedef struct {
    double used_pct;
    double elapsed_pct;
    bool has_elapsed;
} bar_data_t;

typedef struct {
    char label[64];
    char provider[16];
    char right_text[64];
    int64_t total;
    int64_t seg_a; /* cache_read */
    int64_t seg_b; /* cache_write */
    int64_t seg_c; /* input */
    int64_t seg_d; /* output + reasoning */
} hbar_item_t;

typedef struct {
    char title[64];
    bool stacked_segments;
    int n_items;
    hbar_item_t items[12];
} hbar_chart_t;

typedef struct {
    char day[16];
    int64_t claude_tok;
    int64_t codex_tok;
    int64_t agy_tok;
    int64_t turns;
} daily_bar_t;

typedef struct {
    int n_days;
    daily_bar_t days[31];
} daily_chart_t;

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
    bool provider_details_expanded;
} app_ui_t;

typedef struct {
    app_ui_t *ui;
    int days;
    int mask;
    ai_usage_report_t report;
    ai_grouped_quota_t *antigravity_quotas;
    size_t n_antigravity_quotas;
    double elapsed_ms;
} worker_ctx_t;

static void provider_color(const char *prov, double *r, double *g, double *b) {
    if (strcmp(prov, "claude") == 0) {
        *r = 0.88; *g = 0.48; *b = 0.30; /* warm coral/orange */
    } else if (strcmp(prov, "codex") == 0) {
        *r = 0.26; *g = 0.62; *b = 0.92; /* sky blue */
    } else {
        *r = 0.28; *g = 0.76; *b = 0.54; /* emerald green */
    }
}

static const char *provider_name(const char *provider) {
    if (strcmp(provider, "claude") == 0) return "Claude";
    if (strcmp(provider, "codex") == 0) return "Codex";
    if (strcmp(provider, "antigravity") == 0) return "Antigravity";
    return provider;
}

static GtkWidget *card_label(const char *text, const char *css_class) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 32);
    gtk_widget_add_css_class(label, css_class);
    return label;
}

static GtkWidget *provider_title(const char *provider) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dot, "provider-dot");
    gtk_widget_add_css_class(dot, provider);
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), dot);
    gtk_box_append(GTK_BOX(row), card_label(provider_name(provider), "provider-title"));
    return row;
}

static GtkWidget *provider_flow(void) {
    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 3);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 14);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 14);
    return flow;
}

static void chart_ink(GtkDrawingArea *area, cairo_t *cr, double opacity) {
    GdkRGBA foreground;
    gtk_widget_get_color(GTK_WIDGET(area), &foreground);
    cairo_set_source_rgba(cr, foreground.red, foreground.green, foreground.blue,
                          foreground.alpha * opacity);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (w <= 0.0 || h <= 0.0) return;
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

static void draw_quota_bar(
    GtkDrawingArea *area,
    cairo_t *cr,
    int width,
    int height,
    gpointer user_data
) {
    bar_data_t *bd = (bar_data_t *)user_data;
    double r = (double)height / 2.0;

    rounded_rect(cr, 0, 0, width, height, r);
    chart_ink(area, cr, 0.12);
    cairo_fill(cr);

    double clamped = bd->used_pct;
    if (clamped < 0.0) clamped = 0.0;
    if (clamped > 100.0) clamped = 100.0;
    double fill_w = (clamped / 100.0) * (double)width;
    if (fill_w > 1.0) {
        cairo_save(cr);
        rounded_rect(cr, 0, 0, width, height, r);
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
        cairo_rectangle(cr, 0, 0, fill_w < 3.0 ? 3.0 : fill_w, height);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    if (bd->has_elapsed && bd->elapsed_pct >= 0.0 && bd->elapsed_pct <= 100.0) {
        double mx = (bd->elapsed_pct / 100.0) * (double)width;
        if (mx < 2.0) mx = 2.0;
        if (mx > (double)width - 2.0) mx = (double)width - 2.0;
        chart_ink(area, cr, 0.95);
        cairo_set_line_width(cr, 2.2);
        cairo_move_to(cr, mx, 0);
        cairo_line_to(cr, mx, height);
        cairo_stroke(cr);
    }
}

static GtkWidget *make_quota_drawing_area(
    double used_pct,
    bool has_elapsed,
    double elapsed_pct
) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 10);
    gtk_widget_set_hexpand(area, TRUE);
    bar_data_t *bd = g_new0(bar_data_t, 1);
    bd->used_pct = used_pct;
    bd->has_elapsed = has_elapsed;
    bd->elapsed_pct = elapsed_pct;
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_quota_bar, bd, g_free);
    return area;
}

static void draw_hbar_chart(
    GtkDrawingArea *area,
    cairo_t *cr,
    int width,
    int height,
    gpointer user_data
) {
    hbar_chart_t *ch = (hbar_chart_t *)user_data;
    const PangoFontDescription *font = pango_context_get_font_description(
        gtk_widget_get_pango_context(GTK_WIDGET(area)));
    cairo_select_font_face(cr, pango_font_description_get_family(font),
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    if (ch->n_items == 0) {
        cairo_set_font_size(cr, 12.0);
        chart_ink(area, cr, 0.7);
        cairo_move_to(cr, 12, height / 2.0);
        cairo_show_text(cr, "No usage data in selected window");
        return;
    }

    int64_t max_val = 1;
    for (int i = 0; i < ch->n_items; ++i) {
        if (ch->items[i].total > max_val) max_val = ch->items[i].total;
    }

    double top_y = 6.0;
    if (ch->stacked_segments) {
        const char *leg_names[4] = {"Cache Read", "Cache Write", "Uncached In", "Out + Reas"};
        const double leg_cols[4][3] = {
            {0.22, 0.64, 0.88},
            {0.62, 0.44, 0.88},
            {0.92, 0.66, 0.24},
            {0.30, 0.78, 0.52},
        };
        double lx = 4.0;
        cairo_set_font_size(cr, 11.0);
        for (int k = 0; k < 4; ++k) {
            rounded_rect(cr, lx, 4.0, 10.0, 10.0, 3.0);
            cairo_set_source_rgba(cr, leg_cols[k][0], leg_cols[k][1], leg_cols[k][2], 0.92);
            cairo_fill(cr);
            chart_ink(area, cr, 0.85);
            cairo_move_to(cr, lx + 14.0, 13.0);
            cairo_show_text(cr, leg_names[k]);
            lx += 118.0;
        }
        top_y = 26.0;
    } else {
        const char *p_names[3] = {"claude", "codex", "antigravity"};
        double lx = 4.0;
        cairo_set_font_size(cr, 11.0);
        for (int k = 0; k < 3; ++k) {
            double r, g, b;
            provider_color(p_names[k], &r, &g, &b);
            rounded_rect(cr, lx, 4.0, 10.0, 10.0, 3.0);
            cairo_set_source_rgba(cr, r, g, b, 0.92);
            cairo_fill(cr);
            chart_ink(area, cr, 0.85);
            cairo_move_to(cr, lx + 14.0, 13.0);
            cairo_show_text(cr, provider_name(p_names[k]));
            lx += 105.0;
        }
        top_y = 26.0;
    }

    double row_h = 26.0;
    double label_w = 210.0;
    double right_w = 155.0;
    if (width < 600) {
        label_w = 140.0;
        right_w = 120.0;
    }
    double bar_x = label_w + 8.0;
    double bar_max_w = (double)width - bar_x - right_w - 8.0;
    if (bar_max_w < 60.0) bar_max_w = 60.0;

    for (int i = 0; i < ch->n_items; ++i) {
        hbar_item_t *it = &ch->items[i];
        double y = top_y + (double)i * row_h;
        double bar_h = 15.0;
        double by = y + (row_h - bar_h) / 2.0;

        cairo_set_font_size(cr, 11.5);
        chart_ink(area, cr, 0.92);
        cairo_move_to(cr, 4.0, by + 11.5);
        cairo_show_text(cr, it->label);

        rounded_rect(cr, bar_x, by, bar_max_w, bar_h, 5.0);
        chart_ink(area, cr, 0.10);
        cairo_fill(cr);

        double tot_w = ((double)it->total / (double)max_val) * bar_max_w;
        if (tot_w > 0.5 && it->total > 0) {
            if (tot_w < 3.0) tot_w = 3.0;
            cairo_save(cr);
            rounded_rect(cr, bar_x, by, tot_w, bar_h, 5.0);
            cairo_clip(cr);

            if (ch->stacked_segments) {
                const int64_t segs[4] = {it->seg_a, it->seg_b, it->seg_c, it->seg_d};
                const double cols[4][3] = {
                    {0.22, 0.64, 0.88},
                    {0.62, 0.44, 0.88},
                    {0.92, 0.66, 0.24},
                    {0.30, 0.78, 0.52},
                };
                double cur_x = bar_x;
                for (int s = 0; s < 4; ++s) {
                    if (segs[s] <= 0) continue;
                    double sw = ((double)segs[s] / (double)it->total) * tot_w;
                    cairo_set_source_rgba(cr, cols[s][0], cols[s][1], cols[s][2], 0.92);
                    cairo_rectangle(cr, cur_x, by, sw + 0.5, bar_h);
                    cairo_fill(cr);
                    cur_x += sw;
                }
            } else {
                double r, g, b;
                provider_color(it->provider, &r, &g, &b);
                cairo_set_source_rgba(cr, r, g, b, 0.90);
                cairo_rectangle(cr, bar_x, by, tot_w, bar_h);
                cairo_fill(cr);
            }
            cairo_restore(cr);
        }

        cairo_text_extents_t ext;
        cairo_text_extents(cr, it->right_text, &ext);
        chart_ink(area, cr, 0.88);
        cairo_move_to(cr, (double)width - ext.width - 6.0, by + 11.5);
        cairo_show_text(cr, it->right_text);
    }
}

static GtkWidget *make_hbar_chart_card(const char *subtitle, hbar_chart_t *ch) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "card-box");

    GtkWidget *lbl = card_label(subtitle, "metric-title");
    gtk_box_append(GTK_BOX(card), lbl);

    GtkWidget *area = gtk_drawing_area_new();
    int h = 34 + (ch->n_items > 0 ? ch->n_items * 26 : 30);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), h);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_hbar_chart, ch, g_free);
    gtk_box_append(GTK_BOX(card), area);
    return card;
}

static void draw_daily_chart(
    GtkDrawingArea *area,
    cairo_t *cr,
    int width,
    int height,
    gpointer user_data
) {
    daily_chart_t *dc = (daily_chart_t *)user_data;
    const PangoFontDescription *font = pango_context_get_font_description(
        gtk_widget_get_pango_context(GTK_WIDGET(area)));
    cairo_select_font_face(cr, pango_font_description_get_family(font),
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    if (dc->n_days == 0) {
        cairo_set_font_size(cr, 12.0);
        chart_ink(area, cr, 0.7);
        cairo_move_to(cr, 12, height / 2.0);
        cairo_show_text(cr, "No daily usage data in selected window");
        return;
    }

    const char *p_names[3] = {"claude", "codex", "antigravity"};
    double lx = 4.0;
    cairo_set_font_size(cr, 11.0);
    for (int k = 0; k < 3; ++k) {
        double r, g, b;
        provider_color(p_names[k], &r, &g, &b);
        rounded_rect(cr, lx, 4.0, 10.0, 10.0, 3.0);
        cairo_set_source_rgba(cr, r, g, b, 0.92);
        cairo_fill(cr);
        chart_ink(area, cr, 0.85);
        cairo_move_to(cr, lx + 14.0, 13.0);
        cairo_show_text(cr, provider_name(p_names[k]));
        lx += 105.0;
    }

    int64_t max_day = 1;
    for (int i = 0; i < dc->n_days; ++i) {
        int64_t t = dc->days[i].claude_tok + dc->days[i].codex_tok + dc->days[i].agy_tok;
        if (t > max_day) max_day = t;
    }

    double pad_l = 58.0;
    double pad_r = 12.0;
    double pad_t = 30.0;
    double pad_b = 28.0;
    double plot_w = (double)width - pad_l - pad_r;
    double plot_h = (double)height - pad_t - pad_b;
    if (plot_w < 60.0 || plot_h < 40.0) return;

    for (int g = 0; g <= 4; ++g) {
        double frac = (double)g / 4.0;
        double gy = pad_t + plot_h * (1.0 - frac);
        chart_ink(area, cr, 0.12);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, pad_l, gy);
        cairo_line_to(cr, pad_l + plot_w, gy);
        cairo_stroke(cr);

        int64_t gv = (int64_t)llround((double)max_day * frac);
        char gs[24];
        ai_fmt_tokens(gv, gs, sizeof(gs));
        cairo_text_extents_t ext;
        cairo_set_font_size(cr, 10.5);
        cairo_text_extents(cr, gs, &ext);
        chart_ink(area, cr, 0.75);
        cairo_move_to(cr, pad_l - ext.width - 8.0, gy + 4.0);
        cairo_show_text(cr, gs);
    }

    double slot_w = plot_w / (double)dc->n_days;
    double bar_w = slot_w * 0.62;
    if (bar_w > 48.0) bar_w = 48.0;
    if (bar_w < 6.0) bar_w = 6.0;

    for (int i = 0; i < dc->n_days; ++i) {
        daily_bar_t *d = &dc->days[i];
        int64_t tot = d->claude_tok + d->codex_tok + d->agy_tok;
        double cx = pad_l + ((double)i + 0.5) * slot_w;
        double bx = cx - bar_w / 2.0;
        double tot_h = ((double)tot / (double)max_day) * plot_h;

        if (tot > 0 && tot_h > 1.0) {
            if (tot_h < 3.0) tot_h = 3.0;
            double by_top = pad_t + plot_h - tot_h;
            cairo_save(cr);
            rounded_rect(cr, bx, by_top, bar_w, tot_h, 4.0);
            cairo_clip(cr);

            const int64_t vals[3] = {d->claude_tok, d->codex_tok, d->agy_tok};
            double cur_bottom = pad_t + plot_h;
            for (int p = 0; p < 3; ++p) {
                if (vals[p] <= 0) continue;
                double sh = ((double)vals[p] / (double)tot) * tot_h;
                double r, g, b;
                provider_color(p_names[p], &r, &g, &b);
                cairo_set_source_rgba(cr, r, g, b, 0.92);
                cairo_rectangle(cr, bx, cur_bottom - sh - 0.5, bar_w, sh + 0.5);
                cairo_fill(cr);
                cur_bottom -= sh;
            }
            cairo_restore(cr);

            if (dc->n_days <= 14) {
                char ts[24];
                ai_fmt_tokens(tot, ts, sizeof(ts));
                cairo_text_extents_t ext;
                cairo_set_font_size(cr, 10.0);
                cairo_text_extents(cr, ts, &ext);
                chart_ink(area, cr, 0.88);
                cairo_move_to(cr, cx - ext.width / 2.0, by_top - 5.0);
                cairo_show_text(cr, ts);
            }
        }

        int stride = dc->n_days > 16 ? 2 : 1;
        if (i % stride == 0 || i == dc->n_days - 1) {
            const char *short_d = strlen(d->day) >= 10 ? (d->day + 5) : d->day;
            cairo_text_extents_t ext;
            cairo_set_font_size(cr, 10.5);
            cairo_text_extents(cr, short_d, &ext);
            chart_ink(area, cr, 0.85);
            cairo_move_to(cr, cx - ext.width / 2.0, pad_t + plot_h + 16.0);
            cairo_show_text(cr, short_d);
        }
    }
}

static int cmp_daily_bar_asc(const void *a, const void *b) {
    const daily_bar_t *da = (const daily_bar_t *)a;
    const daily_bar_t *db = (const daily_bar_t *)b;
    return strcmp(da->day, db->day);
}

static GtkWidget *make_daily_chart_card(const ai_usage_report_t *rep, int height) {
    daily_chart_t *dc = g_new0(daily_chart_t, 1);

    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        for (int d = 0; d < pu->n_days; ++d) {
            const char *day_key = pu->by_day[d].key;
            int64_t tok = ai_token_stats_total(&pu->by_day[d].stats);
            int idx = -1;
            for (int k = 0; k < dc->n_days; ++k) {
                if (strcmp(dc->days[k].day, day_key) == 0) {
                    idx = k;
                    break;
                }
            }
            if (idx < 0) {
                if (dc->n_days >= 31) continue;
                idx = dc->n_days++;
                snprintf(dc->days[idx].day, sizeof(dc->days[idx].day), "%.15s", day_key);
            }
            if (strcmp(pu->provider, "claude") == 0) dc->days[idx].claude_tok += tok;
            else if (strcmp(pu->provider, "codex") == 0) dc->days[idx].codex_tok += tok;
            else dc->days[idx].agy_tok += tok;
            dc->days[idx].turns += pu->by_day[d].stats.turns;
        }
    }
    if (dc->n_days > 1) {
        qsort(dc->days, (size_t)dc->n_days, sizeof(daily_bar_t), cmp_daily_bar_asc);
    }

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "card-box");

    GtkWidget *lbl = card_label("Daily token burn by provider (stacked)", "metric-title");
    gtk_box_append(GTK_BOX(card), lbl);

    GtkWidget *area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), height);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_daily_chart, dc, g_free);
    gtk_box_append(GTK_BOX(card), area);
    return card;
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

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), grid);
    gtk_box_append(GTK_BOX(card), scrolled);
    return card;
}

static GtkWidget *make_quota_row(const ai_quota_window_t *qw, double now, bool rolling) {
    bool expired = qw->has_reset && qw->reset_epoch <= now;
    double el = 0.0;
    bool has_el = !rolling && !expired && ai_quota_elapsed_pct(qw, now, &el);

    GtkWidget *w_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(w_row, 12);
    char window_label[96];
    snprintf(window_label, sizeof(window_label), rolling ? "%s rolling reference" : "%s window used",
             qw->label);
    gtk_box_append(GTK_BOX(w_row), card_label(window_label, "metric-title"));
    char value[64];
    snprintf(value, sizeof(value), "%.1f%%", qw->used_pct);
    gtk_box_append(GTK_BOX(w_row), card_label(value, "metric-value"));

    GtkWidget *bar = make_quota_drawing_area(qw->used_pct, has_el, el);
    if (expired) gtk_widget_set_opacity(bar, 0.45);
    gtk_box_append(GTK_BOX(w_row), bar);

    char meta_txt[160];
    if (rolling) {
        char tokens[24];
        ai_fmt_tokens(qw->window_tokens, tokens, sizeof(tokens));
        snprintf(meta_txt, sizeof(meta_txt), "%s tokens (%lld turns). Local usage reference, not a provider quota.",
                 tokens, (long long)qw->window_turns);
    } else if (expired) {
        snprintf(meta_txt, sizeof(meta_txt), "Awaiting update. Last reading %.1f%% used.", qw->used_pct);
    } else if (qw->has_reset) {
        char remaining[32];
        ai_fmt_duration(qw->reset_epoch - now, remaining, sizeof(remaining));
        g_autoptr(GDateTime) reset = g_date_time_new_from_unix_local((gint64)qw->reset_epoch);
        g_autofree char *reset_time = reset ? g_date_time_format(reset, "%a %d %b, %H:%M") : NULL;
        snprintf(meta_txt, sizeof(meta_txt), "Resets %s (in %s)",
                 reset_time ? reset_time : "soon", remaining);
    } else {
        snprintf(meta_txt, sizeof(meta_txt), "Reset unknown");
    }
    gtk_box_append(GTK_BOX(w_row), card_label(meta_txt, "metric-sub"));

    if (!rolling && !expired && qw->has_reset) {
        double projected = 0.0;
        double observed_at = qw->has_updated ? MIN(qw->updated_epoch, now) : now;
        if ((!qw->from_history || qw->has_updated) &&
            ai_quota_projected_pct(qw, observed_at, &projected)) {
            snprintf(meta_txt, sizeof(meta_txt), "Pace: %.0f%% by reset", projected);
            gtk_box_append(GTK_BOX(w_row), card_label(meta_txt, "metric-sub"));
        }
    }
    if (qw->from_history) {
        gtk_box_append(GTK_BOX(w_row), card_label("Last recorded reading", "metric-sub"));
    }

    return w_row;
}

static void append_today_usage(GtkWidget *overview, const ai_usage_report_t *rep) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *day = g_date_time_format(now, "%F");
    GtkWidget *heading = gtk_label_new("Today's Token Usage");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_widget_add_css_class(heading, "section-header");
    gtk_box_append(GTK_BOX(overview), heading);

    GtkWidget *flow = provider_flow();
    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *provider = &rep->providers[i];
        int64_t total = 0;
        for (int d = 0; d < provider->n_days; ++d) {
            if (strcmp(provider->by_day[d].key, day) == 0) {
                total = ai_token_stats_total(&provider->by_day[d].stats);
                break;
            }
        }
        char tokens[32];
        ai_fmt_tokens(total, tokens, sizeof(tokens));
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_add_css_class(card, "card-box");
        gtk_widget_set_size_request(card, 280, -1);
        gtk_box_append(GTK_BOX(card), provider_title(provider->provider));
        gtk_box_append(GTK_BOX(card), card_label(tokens, "metric-value"));
        gtk_box_append(GTK_BOX(card), card_label("Tokens today", "metric-sub"));
        gtk_flow_box_append(GTK_FLOW_BOX(flow), card);
    }
    gtk_box_append(GTK_BOX(overview), flow);
}

static void on_provider_details_expanded(GObject *expander, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    app_ui_t *ui = (app_ui_t *)user_data;
    ui->provider_details_expanded = gtk_expander_get_expanded(GTK_EXPANDER(expander));
}

static void populate_overview(app_ui_t *ui, const worker_ctx_t *ctx) {
    const ai_usage_report_t *rep = &ctx->report;
    clear_box(ui->overview_box);
    double now = (double)g_get_real_time() / G_USEC_PER_SEC;

    GtkWidget *q_hdr = gtk_label_new("Quota and Reset Times");
    gtk_label_set_xalign(GTK_LABEL(q_hdr), 0.0f);
    gtk_widget_add_css_class(q_hdr, "section-header");
    gtk_box_append(GTK_BOX(ui->overview_box), q_hdr);

    GtkWidget *q_list = provider_flow();
    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(card, "card-box");
        gtk_widget_set_size_request(card, 280, -1);
        gtk_widget_set_valign(card, GTK_ALIGN_FILL);
        gtk_box_append(GTK_BOX(card), provider_title(pu->provider));

        bool antigravity = strcmp(pu->provider, "antigravity") == 0;
        bool live_quotas = antigravity && ctx->n_antigravity_quotas > 0;
        const char *source = live_quotas ? "Antigravity CLI /usage" : pu->quota_note;
        char src_txt[128];
        snprintf(src_txt, sizeof(src_txt), "source: %s", *source ? source : "-");
        gtk_box_append(GTK_BOX(card), card_label(src_txt, "metric-sub"));

        bool claude = strcmp(pu->provider, "claude") == 0;
        if (live_quotas) {
            const char *previous_group = "";
            for (size_t q = 0; q < ctx->n_antigravity_quotas; ++q) {
                const ai_grouped_quota_t *quota = &ctx->antigravity_quotas[q];
                if (strcmp(previous_group, quota->group) != 0) {
                    GtkWidget *group = card_label(quota->group, "metric-title");
                    gtk_box_append(GTK_BOX(card), group);
                    previous_group = quota->group;
                }
                gtk_box_append(GTK_BOX(card), make_quota_row(&quota->window, now, false));
            }
            gtk_flow_box_append(GTK_FLOW_BOX(q_list), card);
            continue;
        }
        if (antigravity) {
            GtkWidget *empty = card_label("Provider quotas unavailable. Local usage below.", "metric-sub");
            gtk_box_append(GTK_BOX(card), empty);
        }
        const char *claude_labels[] = {"5h", "7d"};
        const ai_quota_window_t *claude_windows[2] = {NULL, NULL};
        if (claude) {
            for (int q = 0; q < pu->n_quotas; ++q) {
                for (int w = 0; w < 2; ++w) {
                    if (strcmp(pu->quotas[q].label, claude_labels[w]) == 0) {
                        claude_windows[w] = &pu->quotas[q];
                    }
                }
            }
        }
        int n_windows = claude ? 2 : pu->n_quotas;
        for (int q = 0; q < n_windows; ++q) {
            const ai_quota_window_t *qw = claude ? claude_windows[q] : &pu->quotas[q];
            if (!qw) {
                char unavailable[48];
                snprintf(unavailable, sizeof(unavailable), "%s quota unavailable", claude_labels[q]);
                GtkWidget *empty = card_label(unavailable, "metric-sub");
                gtk_box_append(GTK_BOX(card), empty);
                continue;
            }
            gtk_box_append(GTK_BOX(card), make_quota_row(qw, now, antigravity));
        }
        if (n_windows == 0) {
            GtkWidget *empty = card_label("No quota data available", "metric-sub");
            gtk_box_append(GTK_BOX(card), empty);
        }
        gtk_flow_box_append(GTK_FLOW_BOX(q_list), card);
    }
    gtk_box_append(GTK_BOX(ui->overview_box), q_list);
    append_today_usage(ui->overview_box, rep);

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

    int64_t grand_tot = ai_token_stats_total(&grand);
    char v0[32], v1[32], v2[32], v3[32], cw_s[32], out_s[32], reas_s[32];
    ai_fmt_tokens(grand_tot, v0, sizeof(v0));
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

    const char *m_titles[4] = {"Total tokens", "Input (uncached)", "Cache read", "Output + reasoning"};
    const char *m_vals[4] = {v0, v1, v2, v3};
    const char *m_subs[4] = {s0, s1, s2, s3};
    for (int k = 0; k < 4; ++k) {
        GtkWidget *cbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_add_css_class(cbox, "card-box");
        GtkWidget *tl = card_label(m_titles[k], "metric-title");
        GtkWidget *vl = card_label(m_vals[k], "metric-value");
        GtkWidget *sl = card_label(m_subs[k], "metric-sub");
        gtk_box_append(GTK_BOX(cbox), tl);
        gtk_box_append(GTK_BOX(cbox), vl);
        gtk_box_append(GTK_BOX(cbox), sl);
        gtk_flow_box_append(GTK_FLOW_BOX(flow), cbox);
    }
    gtk_box_append(GTK_BOX(ui->overview_box), flow);

    /* Provider stacked token composition chart + daily timeline chart */
    hbar_chart_t *prov_ch = g_new0(hbar_chart_t, 1);
    prov_ch->stacked_segments = true;
    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        const ai_token_stats_t *t = &pu->totals;
        int64_t ptot = ai_token_stats_total(t);
        hbar_item_t *it = &prov_ch->items[prov_ch->n_items++];
        snprintf(it->label, sizeof(it->label), "%s (%d sess)", provider_name(pu->provider), pu->sessions_count);
        snprintf(it->provider, sizeof(it->provider), "%s", pu->provider);
        it->total = ptot;
        it->seg_a = t->cache_read_tokens;
        it->seg_b = t->cache_write_tokens;
        it->seg_c = t->input_tokens;
        it->seg_d = t->output_tokens + t->reasoning_tokens;
        char ts[24];
        ai_fmt_tokens(ptot, ts, sizeof(ts));
        double sh = grand_tot > 0 ? (100.0 * (double)ptot / (double)grand_tot) : 0.0;
        snprintf(it->right_text, sizeof(it->right_text), "%7s (%5.1f%%)", ts, sh);
    }
    gtk_box_append(
        GTK_BOX(ui->overview_box),
        make_hbar_chart_card("Provider volume and token composition", prov_ch)
    );
    gtk_box_append(GTK_BOX(ui->overview_box), make_daily_chart_card(rep, 175));

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
        ai_fmt_tokens(grand_tot, buf[n_rows][8], 32);
        for (int c = 0; c < 9; ++c) cells[n_rows][c] = buf[n_rows][c];
        ++n_rows;
    }

    GtkWidget *details = gtk_expander_new("Detailed provider usage");
    gtk_expander_set_expanded(GTK_EXPANDER(details), ui->provider_details_expanded);
    g_signal_connect(details, "notify::expanded", G_CALLBACK(on_provider_details_expanded), ui);
    gtk_expander_set_child(GTK_EXPANDER(details), create_table_card(9, headers, ralign, n_rows, cells));
    gtk_box_append(GTK_BOX(ui->overview_box), details);
}

static int cmp_hbar_item_desc(const void *a, const void *b) {
    const hbar_item_t *ia = (const hbar_item_t *)a;
    const hbar_item_t *ib = (const hbar_item_t *)b;
    if (ia->total > ib->total) return -1;
    if (ia->total < ib->total) return 1;
    return 0;
}

static void populate_models(app_ui_t *ui, const ai_usage_report_t *rep) {
    clear_box(ui->models_box);
    GtkWidget *hdr = gtk_label_new("Usage by Model");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
    gtk_widget_add_css_class(hdr, "section-header");
    gtk_box_append(GTK_BOX(ui->models_box), hdr);

    int64_t grand_tot = 0;
    for (int i = 0; i < rep->n_providers; ++i) {
        grand_tot += ai_token_stats_total(&rep->providers[i].totals);
    }

    hbar_item_t all_m[64];
    int n_all_m = 0;
    const char *headers[7] = {"Provider", "Model", "Turns", "Input", "CacheRead", "Out+Reas", "Total"};
    const bool ralign[7] = {false, false, true, true, true, true, true};
    char buf[64][7][64];
    char *cells[64][9];
    int n_rows = 0;

    for (int i = 0; i < rep->n_providers; ++i) {
        const ai_provider_usage_t *pu = &rep->providers[i];
        for (int m = 0; m < pu->n_models && n_rows < 64; ++m) {
            const ai_token_stats_t *st = &pu->by_model[m].stats;
            int64_t mtot = ai_token_stats_total(st);
            if (n_all_m < 64) {
                hbar_item_t *it = &all_m[n_all_m++];
                memset(it, 0, sizeof(*it));
                snprintf(it->label, sizeof(it->label), "%.26s", pu->by_model[m].key);
                snprintf(it->provider, sizeof(it->provider), "%s", pu->provider);
                it->total = mtot;
                char ts[24];
                ai_fmt_tokens(mtot, ts, sizeof(ts));
                double sh = grand_tot > 0 ? (100.0 * (double)mtot / (double)grand_tot) : 0.0;
                snprintf(it->right_text, sizeof(it->right_text), "%7s (%5.1f%%)", ts, sh);
            }
            snprintf(buf[n_rows][0], 64, "%s", pu->provider);
            snprintf(buf[n_rows][1], 64, "%.63s", pu->by_model[m].key);
            snprintf(buf[n_rows][2], 64, "%lld", (long long)st->turns);
            ai_fmt_tokens(st->input_tokens, buf[n_rows][3], 64);
            ai_fmt_tokens(st->cache_read_tokens, buf[n_rows][4], 64);
            ai_fmt_tokens(st->output_tokens + st->reasoning_tokens, buf[n_rows][5], 64);
            ai_fmt_tokens(mtot, buf[n_rows][6], 64);
            for (int c = 0; c < 7; ++c) cells[n_rows][c] = buf[n_rows][c];
            ++n_rows;
        }
    }

    if (n_all_m > 1) qsort(all_m, (size_t)n_all_m, sizeof(hbar_item_t), cmp_hbar_item_desc);
    hbar_chart_t *mch = g_new0(hbar_chart_t, 1);
    mch->stacked_segments = false;
    mch->n_items = n_all_m < 10 ? n_all_m : 10;
    for (int k = 0; k < mch->n_items; ++k) mch->items[k] = all_m[k];
    gtk_box_append(GTK_BOX(ui->models_box), make_hbar_chart_card("Top models by token volume", mch));

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

    gtk_box_append(GTK_BOX(ui->daily_box), make_daily_chart_card(rep, 230));

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

    hbar_item_t s_items[64];
    int n_sitems = 0;
    for (int i = 0; i < n_ptrs; ++i) {
        const ai_session_info_t *s = ptrs[i];
        hbar_item_t *it = &s_items[n_sitems++];
        memset(it, 0, sizeof(*it));
        const char *loc = s->title;
        if (!loc || !*loc) {
            const char *slash = strrchr(s->cwd, '/');
            loc = (slash && *(slash + 1)) ? (slash + 1) : s->cwd;
        }
        if (!loc || !*loc) loc = s->session_id;
        snprintf(it->label, sizeof(it->label), "%.25s", loc);
        snprintf(it->provider, sizeof(it->provider), "%s", s->provider);
        it->total = s->total_tokens;
        char ts[24];
        ai_fmt_tokens(s->total_tokens, ts, sizeof(ts));
        snprintf(it->right_text, sizeof(it->right_text), "%7s (%3lldt)", ts, (long long)s->turns);
    }
    if (n_sitems > 1) qsort(s_items, (size_t)n_sitems, sizeof(hbar_item_t), cmp_hbar_item_desc);
    hbar_chart_t *sch = g_new0(hbar_chart_t, 1);
    sch->stacked_segments = false;
    sch->n_items = n_sitems < 10 ? n_sitems : 10;
    for (int k = 0; k < sch->n_items; ++k) sch->items[k] = s_items[k];
    gtk_box_append(GTK_BOX(ui->sessions_box), make_hbar_chart_card("Top sessions by token volume", sch));

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

static void refresh_async(app_ui_t *ui);

static gboolean apply_report_idle(gpointer data) {
    worker_ctx_t *ctx = (worker_ctx_t *)data;
    app_ui_t *ui = ctx->ui;

    ui->loading = false;
    if (ctx->days != ui->selected_days || ctx->mask != ui->selected_mask) {
        ai_usage_report_free(&ctx->report);
        free(ctx->antigravity_quotas);
        g_free(ctx);
        refresh_async(ui);
        return G_SOURCE_REMOVE;
    }

    gtk_widget_set_sensitive(ui->refresh_btn, TRUE);
    GDateTime *updated = g_date_time_new_from_unix_local((gint64)ctx->report.generated_at);
    char *status = g_date_time_format(updated, "Updated %H:%M:%S");
    gtk_label_set_text(GTK_LABEL(ui->status_label), status);
    g_free(status);
    g_date_time_unref(updated);
    char timing[64];
    snprintf(timing, sizeof(timing), "Data collection took %.0f ms", ctx->elapsed_ms);
    gtk_widget_set_tooltip_text(ui->status_label, timing);

    populate_overview(ui, ctx);
    populate_models(ui, &ctx->report);
    populate_daily(ui, &ctx->report);
    populate_sessions(ui, &ctx->report);

    ai_usage_report_free(&ctx->report);
    free(ctx->antigravity_quotas);
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

static void collect_antigravity_quotas(worker_ctx_t *ctx) {
    g_autofree char *agy = g_find_program_in_path("agy");
    if (!agy) {
        agy = g_build_filename(g_get_home_dir(), ".local", "bin", "agy", NULL);
        if (!g_file_test(agy, G_FILE_TEST_IS_EXECUTABLE)) return;
    }

    char *argv[] = {"timeout", "--kill-after=1s", "12s", agy,
                    "-p", "/usage", "--output-format", "json", NULL};
    g_autofree char *output = NULL;
    int status;
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, &output, NULL, &status, NULL) &&
        g_spawn_check_wait_status(status, NULL)) {
        ctx->n_antigravity_quotas = ai_antigravity_parse_quotas(output, &ctx->antigravity_quotas);
    }
}

static gpointer worker_thread_fn(gpointer data) {
    worker_ctx_t *ctx = (worker_ctx_t *)data;
    gint64 t0 = g_get_monotonic_time();
    ai_usage_collect(&ctx->report, ctx->days, ctx->mask, 0.0, NULL, NULL, NULL);
    if (ctx->mask & AI_PROVIDER_ANTIGRAVITY) collect_antigravity_quotas(ctx);
    ctx->elapsed_ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
    g_idle_add(apply_report_idle, ctx);
    return NULL;
}

static void refresh_async(app_ui_t *ui) {
    if (ui->loading) return;
    ui->loading = true;
    gtk_widget_set_sensitive(ui->refresh_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(ui->status_label), "Refreshing...");
    gtk_widget_set_tooltip_text(ui->status_label, NULL);

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
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 1040);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 740);

    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(inner, 12);
    gtk_widget_set_margin_bottom(inner, 32);
    gtk_widget_set_margin_start(inner, 20);
    gtk_widget_set_margin_end(inner, 20);

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

        GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
        if (theme) {
            gtk_icon_theme_add_search_path(theme, "/home/riccardo/code/ai/ai-usage/assets");
        }
    }
    gtk_window_set_default_icon_name("com.riccardo.AiUsage");

    app_ui_t *ui = g_new0(app_ui_t, 1);
    ui->selected_days = 7;
    ui->selected_mask = AI_PROVIDER_ALL;

    ui->win = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    gtk_window_set_title(GTK_WINDOW(ui->win), "AI Usage");
    gtk_window_set_icon_name(GTK_WINDOW(ui->win), "com.riccardo.AiUsage");
    gtk_window_set_default_size(GTK_WINDOW(ui->win), 1140, 820);
    gtk_widget_set_size_request(GTK_WIDGET(ui->win), 600, 480);

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_application_window_set_content(ui->win, toolbar);

    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

    GtkWidget *stack = adw_view_stack_new();
    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(stack));
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);

    AdwBreakpoint *compact = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 850px"));
    GValue narrow_policy = G_VALUE_INIT;
    g_value_init(&narrow_policy, ADW_TYPE_VIEW_SWITCHER_POLICY);
    g_value_set_enum(&narrow_policy, ADW_VIEW_SWITCHER_POLICY_NARROW);
    adw_breakpoint_add_setter(compact, G_OBJECT(switcher), "policy", &narrow_policy);
    g_value_unset(&narrow_policy);
    adw_application_window_add_breakpoint(ui->win, compact);

    GtkWidget *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(filters, "filter-toolbar");
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), filters);

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
    gtk_widget_set_tooltip_text(days_box, "Usage period");
    gtk_box_append(GTK_BOX(filters), days_box);

    const char *prov_items[5] = {"All providers", "Claude", "Codex", "Antigravity", NULL};
    GtkWidget *dropdown = gtk_drop_down_new_from_strings(prov_items);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), 0);
    g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_provider_changed), ui);
    gtk_widget_set_tooltip_text(dropdown, "Filter by provider");
    gtk_box_append(GTK_BOX(filters), dropdown);

    ui->refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(ui->refresh_btn, "Refresh usage now");
    g_signal_connect(ui->refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), ui);
    ui->status_label = gtk_label_new("Ready");
    gtk_widget_add_css_class(ui->status_label, "metric-sub");
    gtk_widget_set_hexpand(ui->status_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(ui->status_label), 1.0f);
    gtk_box_append(GTK_BOX(filters), ui->status_label);
    gtk_box_append(GTK_BOX(filters), ui->refresh_btn);

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
    g_set_prgname("com.riccardo.AiUsage");
    g_set_application_name("AI Usage");
    AdwApplication *app = adw_application_new("com.riccardo.AiUsage", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
