#ifndef AI_USAGE_CORE_H
#define AI_USAGE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  AI_PROVIDER_CLAUDE = 1 << 0,
  AI_PROVIDER_CODEX = 1 << 1,
  AI_PROVIDER_ANTIGRAVITY = 1 << 2,
  AI_PROVIDER_OPENCODE = 1 << 3,
  AI_PROVIDER_ALL = AI_PROVIDER_CLAUDE | AI_PROVIDER_CODEX | AI_PROVIDER_ANTIGRAVITY | AI_PROVIDER_OPENCODE
};

typedef struct {
  int64_t turns;
  int64_t input_tokens;
  int64_t cache_read_tokens;
  int64_t cache_write_tokens;
  int64_t output_tokens;
  int64_t reasoning_tokens;
} ai_token_stats_t;

typedef struct {
  char label[16];
  double used_pct;
  int window_seconds;
  double reset_epoch;
  bool has_reset;
  double updated_epoch;
  bool has_updated;
  bool from_history;
  bool is_token_window;
  int64_t window_tokens;
  int64_t window_turns;
} ai_quota_window_t;

typedef struct {
  char group[128];
  ai_quota_window_t window;
} ai_grouped_quota_t;

typedef struct {
  char provider[16];
  char session_id[128];
  double updated_epoch;
  char cwd[512];
  char model[128];
  int64_t turns;
  int64_t total_tokens;
  char title[256];
} ai_session_info_t;

typedef struct {
  char key[128];
  ai_token_stats_t stats;
} ai_keyed_stats_t;

typedef struct {
  char provider[16];
  ai_quota_window_t quotas[4];
  int n_quotas;
  char quota_note[64];
  int sessions_count;
  ai_token_stats_t totals;
  ai_keyed_stats_t *by_model;
  int n_models;
  int cap_models;
  ai_keyed_stats_t *by_day;
  int n_days;
  int cap_days;
  ai_session_info_t *sessions;
  int n_sessions;
  int cap_sessions;
} ai_provider_usage_t;

typedef struct {
  double generated_at;
  int days;
  ai_provider_usage_t providers[4];
  int n_providers;
} ai_usage_report_t;

int64_t ai_token_stats_total(const ai_token_stats_t *t);
void ai_token_stats_add(ai_token_stats_t *dst, const ai_token_stats_t *src);

bool ai_quota_elapsed_pct(const ai_quota_window_t *qw, double now, double *out_pct);
bool ai_quota_projected_pct(const ai_quota_window_t *qw, double now, double *out_pct);
bool ai_quota_projected_tokens(const ai_quota_window_t *qw, double now, int64_t *out_tokens);
size_t ai_antigravity_parse_quotas(const char *json, ai_grouped_quota_t **out);

void ai_fmt_tokens(int64_t n, char *buf, size_t cap);
void ai_fmt_duration(double seconds, char *buf, size_t cap);

void ai_usage_collect(
    ai_usage_report_t *out,
    int days,
    int provider_mask,
    double now_override,
    const char *claude_dir,
    const char *codex_dir,
    const char *gemini_dir,
    const char *opencode_dir
);

void ai_usage_report_free(ai_usage_report_t *r);

char *ai_usage_report_to_json(const ai_usage_report_t *r);

char *ai_usage_collect_json(
    int days,
    int provider_mask,
    double now_override,
    const char *claude_dir,
    const char *codex_dir,
    const char *gemini_dir,
    const char *opencode_dir
);

void ai_usage_free_string(char *s);

#ifdef __cplusplus
}
#endif

#endif
