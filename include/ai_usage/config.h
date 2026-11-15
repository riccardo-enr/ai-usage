#ifndef AI_USAGE_CONFIG_H
#define AI_USAGE_CONFIG_H

#include <stddef.h>

typedef struct {
  int mask;
  char claude_dir[512], codex_dir[512];
  int days;
} ai_config_t;

/* path NULL = $XDG_CONFIG_HOME/ai-usage/config.yaml or ~/.config/...; missing file = defaults.
 * Returns 0 on success; nonzero fills err with "config: <path>:<line>: <why>". */
int ai_config_load(const char *path, ai_config_t *out, char *err, size_t errcap);

#endif
