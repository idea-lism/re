// Validation and code generation for .nest syntax files.

#include "validate_and_gen.h"
#include "darray.h"
#include "peg.h"
#include "vpa.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Helper functions ---

static bool is_vpa_scope_rule(VpaRule* rule) {
  if (rule->is_macro) {
    return false;
  }
  if (rule->is_scope) {
    return true;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(rule->units); i++) {
    if (rule->units[i].kind == VPA_SCOPE) {
      return true;
    }
  }
  return false;
}

static VpaUnit* get_scope_body(VpaRule* rule) {
  if (rule->is_scope) {
    return rule->units;
  }
  for (int32_t i = 0; i < (int32_t)darray_size(rule->units); i++) {
    if (rule->units[i].kind == VPA_SCOPE) {
      return rule->units[i].children;
    }
  }
  return NULL;
}

// --- String set utilities ---

static bool str_set_has(char** set, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(set); i++) {
    if (strcmp(set[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static void str_set_add(char*** set, const char* name) {
  if (!str_set_has(*set, name)) {
    char* dup = strdup(name);
    darray_push(*set, dup);
  }
}

static void str_set_free(char** set) {
  for (int32_t i = 0; i < (int32_t)darray_size(set); i++) {
    free(set[i]);
  }
  darray_del(set);
}

// --- Lookup functions ---

static VpaRule* find_vpa_rule(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    if (!ps->vpa_rules[i].is_macro && strcmp(ps->vpa_rules[i].name, name) == 0) {
      return &ps->vpa_rules[i];
    }
  }
  return NULL;
}

static PegRule* find_peg_rule(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->peg_rules); i++) {
    if (strcmp(ps->peg_rules[i].name, name) == 0) {
      return &ps->peg_rules[i];
    }
  }
  return NULL;
}

// --- Token set collection ---

static void collect_emit_set(ParseState* ps, VpaUnit* units, char*** set, char*** visited) {
  for (int32_t i = 0; i < (int32_t)darray_size(units); i++) {
    VpaUnit* u = &units[i];

    if (u->kind == VPA_REGEXP || u->kind == VPA_STATE) {
      if (u->name && u->name[0]) {
        str_set_add(set, u->name);
      }
    } else if (u->kind == VPA_REF) {
      VpaRule* ref = u->name ? find_vpa_rule(ps, u->name) : NULL;
      if (ref) {
        if (!is_vpa_scope_rule(ref)) {
          if (!str_set_has(*visited, u->name)) {
            char* dup = strdup(u->name);
            darray_push(*visited, dup);
            collect_emit_set(ps, ref->units, set, visited);
            for (int32_t j = 0; j < (int32_t)darray_size(ref->units); j++) {
              VpaUnit* ru = &ref->units[j];
              if ((ru->kind == VPA_REGEXP || ru->kind == VPA_STATE) && (!ru->name || !ru->name[0])) {
                str_set_add(set, ref->name);
              }
            }
          }
        }
      } else if (u->name && u->name[0]) {
        str_set_add(set, u->name);
      }
    }
  }
}

static void collect_peg_used_set(PegUnit* unit, char*** set, ParseState* ps, char*** visited_rules) {
  if (unit->kind == PEG_TOK && unit->name) {
    str_set_add(set, unit->name);
  }
  if (unit->kind == PEG_ID && unit->name) {
    if (!str_set_has(*visited_rules, unit->name)) {
      VpaRule* vr = find_vpa_rule(ps, unit->name);
      if (!vr || !is_vpa_scope_rule(vr)) {
        char* dup = strdup(unit->name);
        darray_push(*visited_rules, dup);
        PegRule* ref = find_peg_rule(ps, unit->name);
        if (ref) {
          collect_peg_used_set(&ref->seq, set, ps, visited_rules);
        }
      }
    }
  }
  for (int32_t i = 0; i < (int32_t)darray_size(unit->children); i++) {
    collect_peg_used_set(&unit->children[i], set, ps, visited_rules);
  }
  if (unit->interlace) {
    collect_peg_used_set(unit->interlace, set, ps, visited_rules);
  }
}

// --- Validation functions ---

static bool is_ignored(ParseState* ps, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(ps->ignores.names); i++) {
    if (strcmp(ps->ignores.names[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static bool validate_token_sets(ParseState* ps) {
  for (int32_t v = 0; v < (int32_t)darray_size(ps->vpa_rules); v++) {
    VpaRule* vpa_rule = &ps->vpa_rules[v];
    if (!is_vpa_scope_rule(vpa_rule)) {
      continue;
    }
    VpaUnit* body = get_scope_body(vpa_rule);
    if (!body) {
      continue;
    }

    bool has_peg = false;
    for (int32_t p = 0; p < (int32_t)darray_size(ps->peg_rules); p++) {
      if (strcmp(ps->peg_rules[p].name, vpa_rule->name) == 0) {
        has_peg = true;
        break;
      }
    }
    if (!has_peg) {
      continue;
    }

    char** emit_set = darray_new(sizeof(char*), 0);
    char** visited = darray_new(sizeof(char*), 0);
    collect_emit_set(ps, body, &emit_set, &visited);
    str_set_free(visited);

    char** filtered_emit = darray_new(sizeof(char*), 0);
    for (int32_t i = 0; i < (int32_t)darray_size(emit_set); i++) {
      if (!is_ignored(ps, emit_set[i])) {
        char* dup = strdup(emit_set[i]);
        darray_push(filtered_emit, dup);
      }
    }
    str_set_free(emit_set);

    char** used_set = darray_new(sizeof(char*), 0);
    PegRule* entry = find_peg_rule(ps, vpa_rule->name);
    if (entry) {
      char** visited_rules = darray_new(sizeof(char*), 0);
      collect_peg_used_set(&entry->seq, &used_set, ps, &visited_rules);
      str_set_free(visited_rules);
    }

    for (int32_t i = 0; i < (int32_t)darray_size(used_set); i++) {
      if (!str_set_has(filtered_emit, used_set[i])) {
        parse_error(ps, "scope '%s': peg uses token @%s not emitted by vpa", vpa_rule->name, used_set[i]);
        str_set_free(filtered_emit);
        str_set_free(used_set);
        return false;
      }
    }

    for (int32_t i = 0; i < (int32_t)darray_size(filtered_emit); i++) {
      if (!str_set_has(used_set, filtered_emit[i])) {
        parse_error(ps, "scope '%s': vpa emits token @%s not used by peg", vpa_rule->name, filtered_emit[i]);
        str_set_free(filtered_emit);
        str_set_free(used_set);
        return false;
      }
    }

    str_set_free(filtered_emit);
    str_set_free(used_set);
  }
  return true;
}

// --- Public API ---

bool validate_and_gen(ParseState* ps, HeaderWriter* hw, IrWriter* iw) {
  auto_tag_branches(ps);
  check_cross_bracket_tags(ps);
  assign_peg_scopes(ps);
  if (!validate(ps)) {
    return false;
  }
  if (parse_has_error(ps)) {
    return false;
  }

  peg_gen(&(PegGenInput){.rules = ps->peg_rules}, hw, iw);
  vpa_gen(&(VpaGenInput){
    .rules = ps->vpa_rules,
    .keywords = ps->keywords,
    .states = ps->states,
    .effects = ps->effects,
    .peg_rules = ps->peg_rules,
    .src = ps->src,
  }, hw, iw);

  return true;
}


static bool _validate(ParseState* ps) {
  bool has_vpa_main = false;
  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    if (!ps->vpa_rules[i].is_macro && strcmp(ps->vpa_rules[i].name, "main") == 0) {
      has_vpa_main = true;
      break;
    }
  }
  if (!has_vpa_main) {
    parse_error(ps, "'main' rule must exist in [[vpa]]");
    return false;
  }

  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    VpaRule* rule = &ps->vpa_rules[i];
    for (int32_t j = 0; j < (int32_t)darray_size(rule->units); j++) {
      VpaUnit* u = &rule->units[j];
      if (u->kind != VPA_STATE || !u->state_name || !u->state_name[0]) {
        continue;
      }
      bool found = false;
      for (int32_t s = 0; s < (int32_t)darray_size(ps->states); s++) {
        if (strcmp(ps->states[s].name, u->state_name) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        parse_error(ps, "state '$%s' used in rule '%s' is not declared", u->state_name, rule->name);
        return false;
      }
    }
  }

  for (int32_t i = 0; i < (int32_t)darray_size(ps->vpa_rules); i++) {
    VpaRule* rule = &ps->vpa_rules[i];
    if (!rule->is_scope || rule->is_macro || strcmp(rule->name, "main") == 0) {
      continue;
    }
    bool has_begin = false;
    bool has_end = false;
    for (int32_t j = 0; j < (int32_t)darray_size(rule->units); j++) {
      VpaUnit* u = &rule->units[j];
      if (u->hook == TOK_HOOK_BEGIN) {
        has_begin = true;
      }
      if (u->hook == TOK_HOOK_END) {
        has_end = true;
      }
      if (u->user_hook && u->user_hook[0]) {
        for (int32_t e = 0; e < (int32_t)darray_size(ps->effects); e++) {
          if (strcmp(ps->effects[e].hook_name, u->user_hook) == 0) {
            for (int32_t ef = 0; ef < (int32_t)darray_size(ps->effects[e].effects); ef++) {
              if (ps->effects[e].effects[ef] == TOK_HOOK_BEGIN) {
                has_begin = true;
              }
              if (ps->effects[e].effects[ef] == TOK_HOOK_END) {
                has_end = true;
              }
            }
          }
        }
      }
    }
    if (has_begin && !has_end) {
      parse_error(ps, "scope '%s' missing .end", rule->name);
      return false;
    }
  }

  if (!validate_token_sets(ps)) {
    return false;
  }

  return true;
}

// --- Public API ---

bool validate_and_gen(ParseState* ps, HeaderWriter* hw, IrWriter* iw) {
  auto_tag_branches(ps);
  check_cross_bracket_tags(ps);
  assign_peg_scopes(ps);
  if (!_validate(ps)) {
    return false;
  }
  if (parse_has_error(ps)) {
    return false;
  }

  peg_gen(&(PegGenInput){.rules = ps->peg_rules}, hw, iw);
  vpa_gen(&(VpaGenInput){
    .rules = ps->vpa_rules,
    .keywords = ps->keywords,
    .states = ps->states,
    .effects = ps->effects,
    .peg_rules = ps->peg_rules,
    .src = ps->src,
  }, hw, iw);

  return true;
}




