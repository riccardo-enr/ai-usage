#include "ai_usage/config.h"

#include "ai_usage/core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

static void expand(const char *src, char *dst, size_t cap) {
  const char *home = getenv("HOME");
  if (src[0] == '~' && (src[1] == '/' || !src[1]) && home) snprintf(dst, cap, "%s%s", home, src + 1);
  else snprintf(dst, cap, "%s", src);
}

static int fail(char *err, size_t cap, const char *path, size_t line, const char *why) {
  snprintf(err, cap, "config: %s:%zu: %s", path, line, why);
  return 1;
}

static const char *scalar(yaml_node_t *n) {
  return n && n->type == YAML_SCALAR_NODE ? (const char *)n->data.scalar.value : NULL;
}

static void warn(const char *path, yaml_node_t *n, const char *what, const char *name) {
  fprintf(stderr, "config: %s:%zu: unknown %s '%s' ignored\n", path, n->start_mark.line + 1, what, name);
}

static int parse_bool(const char *s, int *out) {
  if (!strcmp(s, "true")) *out = 1;
  else if (!strcmp(s, "false")) *out = 0;
  else return -1;
  return 0;
}

static int parse_provider(
    yaml_document_t *doc, yaml_node_t *val, const char *path, int bit, char *dir, ai_config_t *out, char *err,
    size_t cap
) {
  if (val->type != YAML_MAPPING_NODE) return fail(err, cap, path, val->start_mark.line + 1, "provider must be a mapping");
  for (yaml_node_pair_t *p = val->data.mapping.pairs.start; p < val->data.mapping.pairs.top; ++p) {
    yaml_node_t *k = yaml_document_get_node(doc, p->key), *v = yaml_document_get_node(doc, p->value);
    const char *ks = scalar(k), *vs = scalar(v);
    if (!ks) return fail(err, cap, path, k->start_mark.line + 1, "key must be a scalar");
    if (!strcmp(ks, "enabled")) {
      int on;
      if (!vs || parse_bool(vs, &on)) return fail(err, cap, path, v->start_mark.line + 1, "enabled must be true or false");
      if (on) out->mask |= bit;
      else out->mask &= ~bit;
    } else if (!strcmp(ks, "dir")) {
      if (!vs) return fail(err, cap, path, v->start_mark.line + 1, "dir must be a string");
      expand(vs, dir, 512);
    } else warn(path, k, "key", ks);
  }
  return 0;
}

static int apply(yaml_document_t *doc, const char *path, ai_config_t *out, char *err, size_t cap) {
  yaml_node_t *root = yaml_document_get_root_node(doc);
  if (!root) return 0;
  if (root->type != YAML_MAPPING_NODE) return fail(err, cap, path, root->start_mark.line + 1, "top level must be a mapping");
  for (yaml_node_pair_t *p = root->data.mapping.pairs.start; p < root->data.mapping.pairs.top; ++p) {
    yaml_node_t *k = yaml_document_get_node(doc, p->key), *v = yaml_document_get_node(doc, p->value);
    const char *ks = scalar(k), *vs = scalar(v);
    if (!ks) return fail(err, cap, path, k->start_mark.line + 1, "key must be a scalar");
    if (!strcmp(ks, "days")) {
      char *end;
      long d = vs ? strtol(vs, &end, 10) : -1;
      if (!vs || *end || end == vs || d < 0) return fail(err, cap, path, v->start_mark.line + 1, "days must be a non-negative integer");
      out->days = (int)d;
    } else if (!strcmp(ks, "providers")) {
      if (v->type != YAML_MAPPING_NODE) return fail(err, cap, path, v->start_mark.line + 1, "providers must be a mapping");
      for (yaml_node_pair_t *q = v->data.mapping.pairs.start; q < v->data.mapping.pairs.top; ++q) {
        yaml_node_t *pk = yaml_document_get_node(doc, q->key), *pv = yaml_document_get_node(doc, q->value);
        const char *name = scalar(pk);
        if (!name) return fail(err, cap, path, pk->start_mark.line + 1, "provider name must be a scalar");
        int rc = 0;
        if (!strcmp(name, "claude")) rc = parse_provider(doc, pv, path, AI_PROVIDER_CLAUDE, out->claude_dir, out, err, cap);
        else if (!strcmp(name, "codex")) rc = parse_provider(doc, pv, path, AI_PROVIDER_CODEX, out->codex_dir, out, err, cap);
        else warn(path, pk, "provider", name);
        if (rc) return rc;
      }
    } else warn(path, k, "key", ks);
  }
  return 0;
}

int ai_config_load(const char *path, ai_config_t *out, char *err, size_t errcap) {
  out->mask = AI_PROVIDER_ALL;
  out->days = 7;
  expand("~/.claude", out->claude_dir, sizeof(out->claude_dir));
  expand("~/.codex", out->codex_dir, sizeof(out->codex_dir));
  err[0] = 0;

  char def[1024];
  if (!path) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(def, sizeof(def), "%s/ai-usage/config.yaml", xdg);
    else if (home) snprintf(def, sizeof(def), "%s/.config/ai-usage/config.yaml", home);
    else return 0;
    path = def;
  }
  FILE *f = fopen(path, "r");
  if (!f) {
    if (errno == ENOENT) return 0;
    snprintf(err, errcap, "config: %s:0: %s", path, strerror(errno));
    return 1;
  }
  yaml_parser_t parser;
  yaml_document_t doc;
  yaml_parser_initialize(&parser);
  yaml_parser_set_input_file(&parser, f);
  int rc;
  if (!yaml_parser_load(&parser, &doc)) {
    rc = fail(err, errcap, path, parser.problem_mark.line + 1, parser.problem ? parser.problem : "invalid YAML");
  } else {
    rc = apply(&doc, path, out, err, errcap);
    yaml_document_delete(&doc);
  }
  yaml_parser_delete(&parser);
  fclose(f);
  return rc;
}
