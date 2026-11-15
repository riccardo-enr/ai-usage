#include "ai_usage/config.h"
#include "ai_usage/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                                                          \
  do {                                                                                                       \
    if (!(cond)) {                                                                                           \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);                             \
      return 1;                                                                                              \
    }                                                                                                        \
  } while (0)

static const char *tmp = "/tmp/ai_usage_test_config.yaml";

static int load(const char *yaml, ai_config_t *c, char *err, size_t cap) {
  FILE *f = fopen(tmp, "w");
  if (!f) return -1;
  fputs(yaml, f);
  fclose(f);
  return ai_config_load(tmp, c, err, cap);
}

int main(void) {
  ai_config_t c;
  char err[256], expect[600];
  setenv("HOME", "/home/test", 1);

  CHECK(ai_config_load("/nonexistent/config.yaml", &c, err, sizeof(err)) == 0);
  CHECK(c.mask == AI_PROVIDER_ALL && c.days == 7);
  CHECK(!strcmp(c.claude_dir, "/home/test/.claude") && !strcmp(c.codex_dir, "/home/test/.codex"));

  CHECK(load("providers:\n  claude: {enabled: true, dir: /a}\n  codex: {enabled: true, dir: /b}\ndays: 30\n", &c, err, sizeof(err)) == 0);
  CHECK(c.mask == AI_PROVIDER_ALL && c.days == 30 && !strcmp(c.claude_dir, "/a") && !strcmp(c.codex_dir, "/b"));

  CHECK(load("providers:\n  codex:\n    enabled: false\n", &c, err, sizeof(err)) == 0);
  CHECK(c.mask == AI_PROVIDER_CLAUDE);

  CHECK(load("providers:\n  claude: {dir: ~/x}\n", &c, err, sizeof(err)) == 0);
  CHECK(!strcmp(c.claude_dir, "/home/test/x"));

  CHECK(load("days: 7\nproviders: [\n", &c, err, sizeof(err)) != 0);
  snprintf(expect, sizeof(expect), "config: %s:", tmp);
  CHECK(!strncmp(err, expect, strlen(expect)) && atoi(err + strlen(expect)) > 0);

  CHECK(load("days: abc\n", &c, err, sizeof(err)) != 0);
  CHECK(strstr(err, ":1:"));

  CHECK(load("bogus: 1\nproviders:\n  gemini: {enabled: true}\n  claude: {x: 1}\ndays: 3\n", &c, err, sizeof(err)) == 0);
  CHECK(c.days == 3 && c.mask == AI_PROVIDER_ALL);

  remove(tmp);
  puts("config tests passed");
  return 0;
}
