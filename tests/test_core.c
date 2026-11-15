#include "ai_usage/core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                                                          \
  do {                                                                                                       \
    if (!(cond)) {                                                                                           \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);                             \
      return 1;                                                                                              \
    }                                                                                                        \
  } while (0)

static int write_text(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  fputs(content, f);
  fclose(f);
  return 0;
}

int main(void) {
  double now = 1000000.0;
  ai_quota_window_t qw = {
      .label = "5h",
      .used_pct = 25.0,
      .window_seconds = 18000,
      .reset_epoch = now + 9000.0,
      .has_reset = true,
      .updated_epoch = now,
      .has_updated = true,
  };
  double el = 0.0, proj = 0.0;
  CHECK(ai_quota_elapsed_pct(&qw, now, &el) && fabs(el - 50.0) < 1e-6);
  CHECK(ai_quota_projected_pct(&qw, now, &proj) && fabs(proj - 50.0) < 1e-6);

  char tpl[] = "/tmp/ai-usage-test-XXXXXX";
  char *root = mkdtemp(tpl);
  CHECK(root != NULL);
  CHECK(setenv("XDG_RUNTIME_DIR", root, 1) == 0);

  const double now_override = 1794132000.0;
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "mkdir -p '%s/claude/projects/demo' '%s/codex/sessions/2026/11/08'", root, root);
  CHECK(system(cmd) == 0);

  char pbuf[512];
  snprintf(pbuf, sizeof(pbuf), "%s/claude/usage-history.jsonl", root);
  CHECK(
      write_text(
          pbuf,
          "{\"t\":1794131913,\"pct\":12.0,\"reset\":1794146400,\"pct7\":34.0,\"reset7\":1794567600}\n"
          "{\"t\":1794131950,\"pct\":13.0,\"reset\":1794146400,\"pct7\":null,\"reset7\":null}\n"
      ) == 0
  );

  snprintf(pbuf, sizeof(pbuf), "%s/claude/projects/demo/sess1.jsonl", root);
  CHECK(
      write_text(
          pbuf,
          "{\"type\":\"assistant\",\"timestamp\":\"2026-11-08T10:00:00Z\",\"sessionId\":\"s-1\",\"cwd\":\"/"
          "tmp/demo\","
          "\"message\":{\"id\":\"msg_1\",\"model\":\"claude-opus-4-6\",\"usage\":{\"input_tokens\":100,"
          "\"cache_read_input_tokens\":400,\"cache_creation_input_tokens\":50,\"output_tokens\":80,"
          "\"output_tokens_details\":{\"thinking_tokens\":20}}}}\n"
      ) == 0
  );

  snprintf(pbuf, sizeof(pbuf), "%s/codex/sessions/2026/11/08/rollout-1.jsonl", root);
  CHECK(
      write_text(
          pbuf,
          "{\"timestamp\":\"2026-11-08T10:00:00Z\",\"type\":\"session_meta\",\"payload\":{\"session_id\":\"c-"
          "1\",\"cwd\":\"/tmp/codex\"}}\n"
          "{\"timestamp\":\"2026-11-08T10:00:01Z\",\"type\":\"turn_context\",\"payload\":{\"model\":\"gpt-6-"
          "astra\"}}\n"
          "{\"timestamp\":\"2026-11-08T10:00:05Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_"
          "count\","
          "\"info\":{\"total_token_usage\":{\"input_tokens\":1000,\"cached_input_tokens\":600,\"cache_write_"
          "input_tokens\":0,"
          "\"output_tokens\":200,\"reasoning_output_tokens\":50}},"
          "\"rate_limits\":{\"plan_type\":\"plus\",\"primary\":{\"used_percent\":15.0,\"window_minutes\":300,"
          "\"resets_at\":1794150000}}}}\n"
      ) == 0
  );

  char cdir[512], xdir[512];
  snprintf(cdir, sizeof(cdir), "%s/claude", root);
  snprintf(xdir, sizeof(xdir), "%s/codex", root);

  ai_usage_report_t rep;
  ai_usage_collect(&rep, 0, AI_PROVIDER_ALL, now_override, cdir, xdir);
  CHECK(rep.n_providers == 2);
  CHECK(rep.providers[0].n_quotas == 2);
  CHECK(rep.providers[0].quotas[0].used_pct == 13.0);
  CHECK(rep.providers[0].quotas[1].used_pct == 34.0);
  CHECK(rep.providers[0].quotas[1].from_history);
  CHECK(rep.providers[0].totals.output_tokens == 60);
  CHECK(rep.providers[0].totals.reasoning_tokens == 20);
  CHECK(ai_token_stats_total(&rep.providers[0].totals) == 630);
  CHECK(rep.providers[1].totals.input_tokens == 400);
  CHECK(rep.providers[1].totals.cache_read_tokens == 600);
  CHECK(rep.providers[1].totals.output_tokens == 150);
  CHECK(rep.providers[1].totals.reasoning_tokens == 50);
  CHECK(ai_token_stats_total(&rep.providers[1].totals) == 1200);

  char *json = ai_usage_report_to_json(&rep);
  CHECK(json != NULL && strstr(json, "\"claude-opus-4-6\"") != NULL);
  ai_usage_free_string(json);
  ai_usage_report_free(&rep);

  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
  CHECK(system(cmd) == 0);
  printf("All C unit tests passed.\n");
  return 0;
}
