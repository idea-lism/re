// PEG (Parsing Expression Grammar) code generation.
// Generates packrat parser functions in LLVM IR,
// and emits node type definitions to the C header.

#include "peg.h"
#include "bitset.h"
#include "coloring.h"
#include "darray.h"
#include "graph.h"
#include "peg_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char* name;
  int32_t id;
} RuleId;

typedef struct {
  char* scope;
  RuleId* rule_ids;
} PegClosure;

typedef struct {
  Bitset* first_set;
  Bitset* last_set;
} RuleSet;

static int32_t _assign_rule_id(PegClosure* closure, const char* name) {
  int32_t n = (int32_t)darray_size(closure->rule_ids);
  for (int32_t i = 0; i < n; i++) {
    if (strcmp(closure->rule_ids[i].name, name) == 0) {
      return closure->rule_ids[i].id;
    }
  }
  int32_t id = n + 1;
  RuleId rid;
  rid.name = strdup(name);
  rid.id = id;
  darray_push(closure->rule_ids, rid);
  return id;
}

static void _compute_first_set(PegUnit* unit, RuleSet* rule_sets, PegRule* rules, Bitset* out) {
  (void)rule_sets;
  (void)rules;
  if (unit->kind == PEG_TOK) {
    bitset_add_bit(out, 0);
  } else if (unit->kind == PEG_ID) {
    bitset_add_bit(out, 0);
  } else if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n > 0) {
      _compute_first_set(&unit->children[0], rule_sets, rules, out);
    }
  } else if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    for (int32_t i = 0; i < n; i++) {
      _compute_first_set(&unit->children[i], rule_sets, rules, out);
    }
  }
}

static void _compute_last_set(PegUnit* unit, RuleSet* rule_sets, PegRule* rules, Bitset* out) {
  (void)rule_sets;
  (void)rules;
  if (unit->kind == PEG_TOK) {
    bitset_add_bit(out, 0);
  } else if (unit->kind == PEG_ID) {
    bitset_add_bit(out, 0);
  } else if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n > 0) {
      _compute_last_set(&unit->children[n - 1], rule_sets, rules, out);
    }
  } else if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    for (int32_t i = 0; i < n; i++) {
      _compute_last_set(&unit->children[i], rule_sets, rules, out);
    }
  }
}

static int32_t _are_exclusive(RuleSet* a, RuleSet* b) {
  Bitset* first_inter = bitset_and(a->first_set, b->first_set);
  int32_t first_empty = bitset_size(first_inter) == 0;
  bitset_del(first_inter);
  if (first_empty) {
    return 1;
  }
  Bitset* last_inter = bitset_and(a->last_set, b->last_set);
  int32_t last_empty = bitset_size(last_inter) == 0;
  bitset_del(last_inter);
  return last_empty;
}

static Graph* _build_interference_graph(RuleSet* rule_sets, int32_t n_rules) {
  Graph* g = graph_new(n_rules);
  for (int32_t i = 0; i < n_rules; i++) {
    for (int32_t j = i + 1; j < n_rules; j++) {
      if (!_are_exclusive(&rule_sets[i], &rule_sets[j])) {
        graph_add_edge(g, i, j);
      }
    }
  }
  return g;
}

static void _gen_ref_type(HeaderWriter* hw) {
  hw_blank(hw);
  hw_raw(hw, "#include <stdint.h>\n");
  hw_raw(hw, "#include <stdbool.h>\n");
  hw_blank(hw);
  hw_comment(hw, "PEG reference type");
  hw_struct_begin(hw, "PegRef");
  hw_field(hw, "void*", "table");
  hw_field(hw, "int32_t", "col");
  hw_field(hw, "int32_t", "next_col");
  hw_struct_end(hw);
  hw_raw(hw, " PegRef;\n\n");
}

static void _gen_node_type(HeaderWriter* hw, PegRule* rule) {
  int32_t name_len = snprintf(NULL, 0, "%sNode", rule->name) + 1;
  char struct_name[name_len];
  snprintf(struct_name, (size_t)name_len, "%sNode", rule->name);
  if (struct_name[0] >= 'a' && struct_name[0] <= 'z') {
    struct_name[0] -= 32;
  }

  hw_struct_begin(hw, struct_name);

  PegUnit** all_branches = darray_new(sizeof(PegUnit*), 0);
  for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
    if (rule->seq.children[i].kind == PEG_BRANCHES) {
      PegUnit* bu = &rule->seq.children[i];
      for (int32_t j = 0; j < (int32_t)darray_size(bu->children); j++) {
        darray_push(all_branches, &bu->children[j]);
      }
    }
  }
  int32_t nbranches = (int32_t)darray_size(all_branches);

  if (nbranches > 0) {
    hw_raw(hw, "  struct {\n");
    for (int32_t i = 0; i < nbranches; i++) {
      PegUnit* branch = all_branches[i];
      if (branch->tag && branch->tag[0]) {
        hw_fmt(hw, "    bool %s : 1;\n", branch->tag);
      } else {
        hw_fmt(hw, "    bool branch%d : 1;\n", i);
      }
    }
    hw_raw(hw, "  } is;\n");

    for (int32_t i = 0; i < nbranches; i++) {
      PegUnit* branch = all_branches[i];
      for (int32_t j = 0; j < (int32_t)darray_size(branch->children); j++) {
        PegUnit* child = &branch->children[j];
        if (child->name && child->name[0]) {
          hw_field(hw, "PegRef", child->name);
        }
      }
    }
  } else {
    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      PegUnit* child = &rule->seq.children[i];
      if (child->name && child->name[0]) {
        hw_field(hw, "PegRef", child->name);
      }
    }
  }

  darray_del(all_branches);
  hw_struct_end(hw);
  hw_fmt(hw, " %s;\n\n", struct_name);
}

static void _gen_load_decl(HeaderWriter* hw, PegRule* rule) {
  int32_t sn_len = snprintf(NULL, 0, "%sNode", rule->name) + 1;
  int32_t fn_len = snprintf(NULL, 0, "load_%s", rule->name) + 1;
  char struct_name[sn_len], func_name[fn_len];
  snprintf(struct_name, (size_t)sn_len, "%sNode", rule->name);
  snprintf(func_name, (size_t)fn_len, "load_%s", rule->name);
  if (struct_name[0] >= 'a' && struct_name[0] <= 'z') {
    struct_name[0] -= 32;
  }
  hw_fmt(hw, "%s %s(PegRef ref);\n", struct_name, func_name);
}

static void _gen_naive_table_struct(HeaderWriter* hw, int32_t n_rules) {
  hw_blank(hw);
  hw_comment(hw, "Naive mode: one slot per rule");
  hw_struct_begin(hw, "Col");
  hw_fmt(hw, "  int32_t slots[%d];\n", n_rules);
  hw_struct_end(hw);
  hw_raw(hw, " Col;\n\n");
}

static void _gen_row_shared_table_struct(HeaderWriter* hw, int32_t sg_size, int32_t n_colors) {
  hw_blank(hw);
  hw_comment(hw, "Row-shared mode: bitset + colored slots");
  hw_struct_begin(hw, "Col");
  hw_fmt(hw, "  int32_t bits[%d];\n", sg_size);
  hw_fmt(hw, "  int32_t slots[%d];\n", n_colors);
  hw_struct_end(hw);
  hw_raw(hw, " Col;\n\n");
}

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w) {
  PegRule* rules = input->rules;
  int32_t n_rules = (int32_t)darray_size(rules);

  if (n_rules == 0) {
    return;
  }

  PegClosure closure = {0};
  closure.rule_ids = darray_new(sizeof(RuleId), 0);
  closure.scope = strdup("main");

  for (int32_t i = 0; i < n_rules; i++) {
    _assign_rule_id(&closure, rules[i].name);
  }

  RuleSet* rule_sets = calloc((size_t)n_rules, sizeof(RuleSet));
  for (int32_t i = 0; i < n_rules; i++) {
    rule_sets[i].first_set = bitset_new();
    rule_sets[i].last_set = bitset_new();
  }

  for (int32_t i = 0; i < n_rules; i++) {
    _compute_first_set(&rules[i].seq, rule_sets, rules, rule_sets[i].first_set);
    _compute_last_set(&rules[i].seq, rule_sets, rules, rule_sets[i].last_set);
  }

  _gen_ref_type(hw);

  for (int32_t i = 0; i < n_rules; i++) {
    _gen_node_type(hw, &rules[i]);
    _gen_load_decl(hw, &rules[i]);
  }

  if (input->mode == PEG_MODE_NAIVE) {
    _gen_naive_table_struct(hw, n_rules);
  } else {
    Graph* g = _build_interference_graph(rule_sets, n_rules);
    int32_t* edges = graph_edges(g);
    int32_t n_edges = graph_n_edges(g);
    
    ColoringResult* cr = coloring_solve(n_rules, edges, n_edges, n_rules, 1000000, 42);
    int32_t sg_size = coloring_get_sg_size(cr);
    
    int32_t max_color = 0;
    for (int32_t i = 0; i < n_rules; i++) {
      int32_t sg_id, seg_mask;
      coloring_get_segment_info(cr, i, &sg_id, &seg_mask);
      (void)seg_mask;
      if (sg_id > max_color) {
        max_color = sg_id;
      }
    }
    
    _gen_row_shared_table_struct(hw, sg_size, max_color + 1);
    
    coloring_result_del(cr);
    graph_del(g);
  }

  hw_blank(hw);
  hw_comment(hw, "Helper functions");
  hw_raw(hw, "static inline bool peg_has_next(PegRef ref) { return ref.next_col >= 0; }\n");
  hw_raw(hw, "static inline PegRef peg_get_next(PegRef ref) { return (PegRef){ref.table, ref.next_col, -1}; }\n");

  (void)w;

  for (int32_t i = 0; i < n_rules; i++) {
    bitset_del(rule_sets[i].first_set);
    bitset_del(rule_sets[i].last_set);
  }
  free(rule_sets);

  for (int32_t i = 0; i < (int32_t)darray_size(closure.rule_ids); i++) {
    free(closure.rule_ids[i].name);
  }
  darray_del(closure.rule_ids);
  free(closure.scope);
}

