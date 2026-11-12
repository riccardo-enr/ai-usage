#include "ai_usage/core.h"

#include <math.h>
#include <sqlite3.h>
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

static const ai_keyed_stats_t *find_model(const ai_provider_usage_t *p, const char *key) {
  for (int i = 0; i < p->n_models; ++i) {
    if (strcmp(p->by_model[i].key, key) == 0) return &p->by_model[i];
  }
  return NULL;
}

static const ai_session_info_t *find_session(const ai_provider_usage_t *p, const char *title) {
  for (int i = 0; i < p->n_sessions; ++i) {
    if (strcmp(p->sessions[i].title, title) == 0) return &p->sessions[i];
  }
  return NULL;
}

/* opencode fixture: assistant tokens.output EXCLUDES reasoning (msg-1: output 200 plus reasoning 40). */
static const char oc_v2_schema[] =
    "CREATE TABLE session_v2(id,title,directory,time_updated);"
    "CREATE TABLE session_message(id,session_id,type,seq,time_created,time_updated,data);";

static int oc_v2_session(
    sqlite3 *db, const char *id, const char *title, const char *directory, long long updated
) {
  char sql[1024];
  snprintf(
      sql, sizeof(sql), "INSERT INTO session_v2 VALUES('%s','%s','%s',%lld);", id, title, directory, updated
  );
  return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static int oc_v2_turn(
    sqlite3 *db,
    const char *id,
    const char *session_id,
    const char *type,
    int seq,
    long long created,
    const char *provider,
    const char *model,
    long long input,
    long long output,
    long long reasoning,
    long long cache_read,
    long long cache_write
) {
  char sql[2048];
  snprintf(
      sql,
      sizeof(sql),
      "INSERT INTO session_message VALUES('%s','%s','%s',%d,%lld,%lld,"
      "'{\"time\":{\"created\":%lld},\"model\":{\"providerID\":\"%s\",\"id\":\"%s\"},"
      "\"tokens\":{\"input\":%lld,\"output\":%lld,\"reasoning\":%lld,"
      "\"cache\":{\"read\":%lld,\"write\":%lld}}}');",
      id,
      session_id,
      type,
      seq,
      created,
      created,
      created,
      provider,
      model,
      input,
      output,
      reasoning,
      cache_read,
      cache_write
  );
  return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static const char oc_legacy_sql[] =
    "CREATE TABLE message(id, session_id, time_created, time_updated, data);"
    "CREATE TABLE session(id, title, directory, time_updated, parent_id);"
    "INSERT INTO session VALUES('ses-legacy','legacy session','/home/test/legacy',%lld,NULL);"
    "INSERT INTO message VALUES('legacy-1','ses-legacy',%lld,%lld,"
    "'{\"role\":\"assistant\",\"providerID\":\"anthropic\",\"modelID\":\"claude-opus-4-6\","
    "\"tokens\":{\"input\":1,\"output\":2,\"reasoning\":0,\"cache\":{\"read\":3,\"write\":0}},"
    "\"time\":{\"created\":%lld}}');";

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

  ai_grouped_quota_t *quotas = NULL;
  size_t n_quotas = ai_antigravity_parse_quotas(
      "{\"command\":{\"data\":{\"groups\":["
      "{\"name\":\"Gemini Models\",\"buckets\":["
      "{\"window\":\"weekly\",\"remaining_fraction\":0.94,\"reset_time\":\"2026-11-15T10:19:49Z\"},"
      "{\"window\":\"5h\",\"remaining_fraction\":1,\"reset_time\":\"2026-11-09T00:39:19Z\"}]},"
      "{\"name\":\"Gemini 4 Argon\",\"buckets\":["
      "{\"window\":\"weekly\",\"remaining_fraction\":0,\"reset_time\":null}]}]}}}",
      &quotas
  );
  CHECK(n_quotas == 3);
  CHECK(strcmp(quotas[0].group, "Gemini Models") == 0);
  CHECK(strcmp(quotas[0].window.label, "5h") == 0);
  CHECK(strcmp(quotas[1].window.label, "7d") == 0);
  CHECK(fabs(quotas[1].window.used_pct - 6.0) < 1e-6);
  CHECK(quotas[0].window.has_reset);
  CHECK(ai_quota_elapsed_pct(&quotas[0].window, quotas[0].window.reset_epoch - 9000, &el));
  CHECK(fabs(el - 50.0) < 1e-6);
  CHECK(strcmp(quotas[2].group, "Gemini 4 Argon") == 0);
  CHECK(quotas[2].window.used_pct == 100.0 && !quotas[2].window.has_reset);
  free(quotas);

  char tpl[] = "/tmp/ai-usage-test-XXXXXX";
  char *root = mkdtemp(tpl);
  CHECK(root != NULL);
  CHECK(setenv("XDG_RUNTIME_DIR", root, 1) == 0);

  char cache_root[512], models_path[640];
  snprintf(cache_root, sizeof(cache_root), "%s/cache", root);
  CHECK(mkdir(cache_root, 0700) == 0);
  snprintf(models_path, sizeof(models_path), "%s/opencode", cache_root);
  CHECK(mkdir(models_path, 0700) == 0);
  snprintf(models_path, sizeof(models_path), "%s/opencode/models.json", cache_root);
  CHECK(
      write_text(
          models_path,
          "{\"opencode-go\":{\"models\":{\"qwen3.8-flash\":{\"cost\":"
          "{\"input\":10000,\"output\":20000,\"cache_read\":5000,\"cache_write\":40000}}}}}"
      ) == 0
  );
  CHECK(setenv("XDG_CACHE_HOME", cache_root, 1) == 0);

  const double now_override = 1794132000.0;
  const long long oc_now_ms = (long long)(now_override * 1000.0);
  char cmd[512];
  snprintf(
      cmd,
      sizeof(cmd),
      "mkdir -p '%s/claude/projects/demo' '%s/codex/sessions/2026/11/08' "
      "'%s/gemini/antigravity/brain/conv-1/.system_generated/logs' '%s/opencode' '%s/opencode-legacy'",
      root,
      root,
      root,
      root,
      root
  );
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

  snprintf(
      pbuf, sizeof(pbuf), "%s/gemini/antigravity/brain/conv-1/.system_generated/logs/transcript.jsonl", root
  );
  CHECK(
      write_text(
          pbuf,
          "{\"step_index\":1,\"type\":\"PLANNER_RESPONSE\",\"created_at\":\"2026-11-08T10:05:00Z\","
          "\"input_tokens\":500,\"cache_read_tokens\":1500,\"output_tokens\":120}\n"
      ) == 0
  );

  snprintf(pbuf, sizeof(pbuf), "%s/opencode/opencode.db", root);
  sqlite3 *db = NULL;
  CHECK(sqlite3_open(pbuf, &db) == SQLITE_OK);
  CHECK(sqlite3_exec(db, oc_v2_schema, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(
      oc_v2_session(db, "ses-aaa", "alpha session", "/home/test/alpha", oc_now_ms - 3600000LL) == SQLITE_OK
  );
  CHECK(
      oc_v2_session(db, "ses-bbb", "beta session", "/home/test/beta", oc_now_ms - 259200000LL) == SQLITE_OK
  );
  CHECK(
      oc_v2_turn(
          db,
          "msg-1",
          "ses-aaa",
          "assistant",
          1,
          oc_now_ms - 3600000LL,
          "anthropic",
          "claude-opus-4-6",
          100,
          200,
          40,
          300,
          50
      ) == SQLITE_OK
  );
  CHECK(
      oc_v2_turn(
          db,
          "msg-2",
          "ses-aaa",
          "assistant",
          2,
          oc_now_ms - 3600000LL,
          "opencode-go",
          "qwen3.8-flash",
          10,
          20,
          0,
          5,
          0
      ) == SQLITE_OK
  );
  CHECK(
      oc_v2_turn(
          db,
          "msg-3",
          "ses-bbb",
          "assistant",
          1,
          oc_now_ms - 259200000LL,
          "opencode-go",
          "qwen3.8-flash",
          7,
          17,
          5,
          11,
          13
      ) == SQLITE_OK
  );
  CHECK(
      oc_v2_turn(
          db,
          "msg-4",
          "ses-bbb",
          "assistant",
          2,
          oc_now_ms - 259200000LL,
          "anthropic",
          "claude-opus-4-6",
          1,
          2,
          0,
          0,
          0
      ) == SQLITE_OK
  );
  CHECK(
      oc_v2_turn(
          db,
          "msg-5",
          "ses-bbb",
          "user",
          3,
          oc_now_ms,
          "anthropic",
          "claude-opus-4-6",
          999,
          999,
          999,
          999,
          999
      ) == SQLITE_OK
  );
  CHECK(sqlite3_close(db) == SQLITE_OK);

  char cdir[512], xdir[512], gdir[512], odir[512], ldir[512];
  snprintf(cdir, sizeof(cdir), "%s/claude", root);
  snprintf(xdir, sizeof(xdir), "%s/codex", root);
  snprintf(gdir, sizeof(gdir), "%s/gemini", root);
  snprintf(odir, sizeof(odir), "%s/opencode", root);
  snprintf(ldir, sizeof(ldir), "%s/opencode-legacy", root);

  ai_usage_report_t rep;
  ai_usage_collect(&rep, 0, AI_PROVIDER_ALL, now_override, cdir, xdir, gdir, odir);
  CHECK(rep.n_providers == 4);
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
  CHECK(ai_token_stats_total(&rep.providers[2].totals) == 2120);

  int oc = -1;
  for (int i = 0; i < rep.n_providers; ++i) {
    if (strcmp(rep.providers[i].provider, "opencode") == 0) oc = i;
  }
  CHECK(oc >= 0);
  const ai_provider_usage_t *op = &rep.providers[oc];
  CHECK(op->n_quotas == 2);
  CHECK(strcmp(op->quotas[0].label, "5h") == 0);
  CHECK(!op->quotas[0].is_token_window);
  CHECK(op->quotas[0].window_turns == 1);
  CHECK(op->quotas[0].window_tokens == 35);
  CHECK(fabs(op->quotas[0].used_pct - 4.375) < 1e-6);
  CHECK(strcmp(op->quotas[1].label, "7d") == 0);
  CHECK(!op->quotas[1].is_token_window);
  CHECK(op->quotas[1].window_turns == 2);
  CHECK(op->quotas[1].window_tokens == 88);
  CHECK(fabs(op->quotas[1].used_pct - 1.61 / 30.0 * 100.0) < 1e-6);
  CHECK(strncmp(op->quota_note, "est. from tokens", 16) == 0);
  CHECK(op->totals.turns == 4);
  CHECK(op->totals.input_tokens == 118);
  CHECK(op->totals.cache_read_tokens == 316);
  CHECK(op->totals.cache_write_tokens == 63);
  CHECK(op->totals.output_tokens == 239);
  CHECK(op->totals.reasoning_tokens == 45);
  CHECK(ai_token_stats_total(&op->totals) == 781);
  CHECK(op->n_models == 2);
  const ai_keyed_stats_t *m1 = find_model(op, "anthropic/claude-opus-4-6");
  CHECK(m1 != NULL);
  CHECK(m1->stats.input_tokens == 101 && m1->stats.output_tokens == 202);
  CHECK(m1->stats.reasoning_tokens == 40 && m1->stats.turns == 2);
  const ai_keyed_stats_t *m2 = find_model(op, "opencode-go/qwen3.8-flash");
  CHECK(m2 != NULL);
  CHECK(m2->stats.input_tokens == 17 && m2->stats.cache_read_tokens == 16);
  CHECK(m2->stats.cache_write_tokens == 13 && m2->stats.reasoning_tokens == 5);
  CHECK(m2->stats.output_tokens == 37 && m2->stats.turns == 2);
  CHECK(op->n_days == 2);
  CHECK(op->n_sessions == 2);
  CHECK(op->sessions_count == 2);
  const ai_session_info_t *sa = find_session(op, "alpha session");
  CHECK(sa != NULL && strcmp(sa->cwd, "/home/test/alpha") == 0);
  CHECK(strcmp(sa->provider, "opencode") == 0);
  CHECK(find_session(op, "beta session") != NULL);

  char *json = ai_usage_report_to_json(&rep);
  CHECK(json != NULL && strstr(json, "\"claude-opus-4-6\"") != NULL);
  CHECK(strstr(json, "\"anthropic/claude-opus-4-6\"") != NULL);
  ai_usage_free_string(json);
  ai_usage_report_free(&rep);

  snprintf(pbuf, sizeof(pbuf), "%s/opencode-legacy/opencode.db", root);
  char legacy_sql[2048];
  snprintf(legacy_sql, sizeof(legacy_sql), oc_legacy_sql, oc_now_ms, oc_now_ms, oc_now_ms, oc_now_ms);
  CHECK(sqlite3_open(pbuf, &db) == SQLITE_OK);
  CHECK(sqlite3_exec(db, legacy_sql, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_close(db) == SQLITE_OK);
  ai_usage_report_t legacy;
  ai_usage_collect(&legacy, 0, AI_PROVIDER_OPENCODE, now_override, NULL, NULL, NULL, ldir);
  CHECK(legacy.n_providers == 1);
  CHECK(legacy.providers[0].totals.turns == 1);
  CHECK(legacy.providers[0].totals.input_tokens == 1);
  CHECK(legacy.providers[0].totals.cache_read_tokens == 3);
  CHECK(legacy.providers[0].totals.output_tokens == 2);
  CHECK(ai_token_stats_total(&legacy.providers[0].totals) == 6);
  CHECK(find_model(&legacy.providers[0], "anthropic/claude-opus-4-6") != NULL);
  CHECK(legacy.providers[0].n_quotas == 0);
  CHECK(strcmp(legacy.providers[0].quota_note, "no quota (BYO providers)") == 0);
  ai_usage_report_free(&legacy);

  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
  CHECK(system(cmd) == 0);
  printf("All C unit tests passed.\n");
  return 0;
}
