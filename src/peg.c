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
  Bitset* first_set;
  Bitset* last_set;
  int32_t slot_idx;
  int32_t sg_id;
  int32_t seg_mask;
  int32_t seg_full_mask;
  char* scope;
  PegRule* rule;
} RuleSet;

typedef struct {
  char* scope_name;
  int32_t* rule_indices;
  int32_t n_rules;
} ScopeInfo;

static int32_t _token_id(char** tokens, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(tokens); i++) {
    if (strcmp(tokens[i], name) == 0) {
      return i + 1;
    }
  }
  darray_push(tokens, strdup(name));
  return (int32_t)darray_size(tokens);
}

static const char* _scope_name(PegRule* rule) {
  return (rule->scope && rule->scope[0]) ? rule->scope : "main";
}

static int32_t _scope_index(ScopeInfo* scopes, const char* name) {
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    if (strcmp(scopes[i].scope_name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static ScopeInfo* _collect_scopes(PegRule* rules, int32_t n_rules) {
  ScopeInfo* scopes = darray_new(sizeof(ScopeInfo), 0);
  for (int32_t i = 0; i < n_rules; i++) {
    const char* name = _scope_name(&rules[i]);
    int32_t si = _scope_index(scopes, name);
    if (si < 0) {
      ScopeInfo scope = {0};
      scope.scope_name = strdup(name);
      scope.rule_indices = darray_new(sizeof(int32_t), 0);
      darray_push(scopes, scope);
      si = (int32_t)darray_size(scopes) - 1;
    }
    darray_push(scopes[si].rule_indices, i);
    scopes[si].n_rules = (int32_t)darray_size(scopes[si].rule_indices);
  }
  return scopes;
}

static void _free_scopes(ScopeInfo* scopes) {
  for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
    free(scopes[i].scope_name);
    darray_del(scopes[i].rule_indices);
  }
  darray_del(scopes);
}

static void _compute_first_set(PegUnit* unit, Bitset* out, RuleSet* rule_sets, int32_t n_rules, Bitset* visited,
                               char** tokens) {
  if (unit->kind == PEG_TOK) {
    bitset_add_bit(out, (uint32_t)_token_id(tokens, unit->name));
  } else if (unit->kind == PEG_ID) {
    for (int32_t i = 0; i < n_rules; i++) {
      if (rule_sets[i].rule && strcmp(rule_sets[i].rule->name, unit->name) == 0) {
        if (!bitset_contains(visited, (uint32_t)i)) {
          bitset_add_bit(visited, (uint32_t)i);
          _compute_first_set(&rule_sets[i].rule->seq, out, rule_sets, n_rules, visited, tokens);
        }
        break;
      }
    }
  } else if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n > 0) {
      _compute_first_set(&unit->children[0], out, rule_sets, n_rules, visited, tokens);
    }
  } else if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    for (int32_t i = 0; i < n; i++) {
      _compute_first_set(&unit->children[i], out, rule_sets, n_rules, visited, tokens);
    }
  }
}

static void _compute_last_set(PegUnit* unit, Bitset* out, RuleSet* rule_sets, int32_t n_rules, Bitset* visited,
                              char** tokens) {
  if (unit->kind == PEG_TOK) {
    bitset_add_bit(out, (uint32_t)_token_id(tokens, unit->name));
  } else if (unit->kind == PEG_ID) {
    for (int32_t i = 0; i < n_rules; i++) {
      if (rule_sets[i].rule && strcmp(rule_sets[i].rule->name, unit->name) == 0) {
        if (!bitset_contains(visited, (uint32_t)i)) {
          bitset_add_bit(visited, (uint32_t)i);
          _compute_last_set(&rule_sets[i].rule->seq, out, rule_sets, n_rules, visited, tokens);
        }
        break;
      }
    }
  } else if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n > 0) {
      _compute_last_set(&unit->children[n - 1], out, rule_sets, n_rules, visited, tokens);
    }
  } else if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    for (int32_t i = 0; i < n; i++) {
      _compute_last_set(&unit->children[i], out, rule_sets, n_rules, visited, tokens);
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
      if (strcmp(_scope_name(rule_sets[i].rule), _scope_name(rule_sets[j].rule)) != 0) {
        continue;
      }
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

static void _gen_load_impl(HeaderWriter* hw, PegRule* rule) {
  int32_t sn_len = snprintf(NULL, 0, "%sNode", rule->name) + 1;
  int32_t fn_len = snprintf(NULL, 0, "load_%s", rule->name) + 1;
  char struct_name[sn_len], func_name[fn_len];
  snprintf(struct_name, (size_t)sn_len, "%sNode", rule->name);
  snprintf(func_name, (size_t)fn_len, "load_%s", rule->name);
  if (struct_name[0] >= 'a' && struct_name[0] <= 'z') {
    struct_name[0] -= 32;
  }
  
  hw_blank(hw);
  hw_fmt(hw, "static inline %s %s(PegRef ref) {\n", struct_name, func_name);
  hw_fmt(hw, "  %s node = {0};\n", struct_name);
  hw_fmt(hw, "  Col* table = (Col*)ref.table;\n");
  hw_fmt(hw, "  int32_t col = ref.col;\n");
  hw_fmt(hw, "  int32_t cur = col;\n");
  
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
    hw_raw(hw, "  // branch reconstruction is conservative for now\n");
    hw_raw(hw, "  (void)table;\n");
    hw_raw(hw, "  (void)cur;\n");
  } else {
    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      PegUnit* child = &rule->seq.children[i];
      if (child->name && child->name[0]) {
        hw_fmt(hw, "  node.%s = (PegRef){table, cur, -1};\n", child->name);
      }

      if (child->kind == PEG_ID && child->name && child->name[0]) {
        hw_fmt(hw, "  int32_t len_%d = parse_%s((void*)table, cur);\n", i, child->name);
        hw_fmt(hw, "  if (len_%d < 0) len_%d = 0;\n", i, i);
        if (child->multiplier == '+' || child->multiplier == '*') {
          hw_fmt(hw, "  node.%s.next_col = cur + len_%d;\n", child->name, i);
        }
        hw_fmt(hw, "  cur += len_%d;\n", i);
      } else if (child->kind == PEG_ID) {
        hw_fmt(hw, "  int32_t len_%d = parse_%s((void*)table, cur);\n", i, child->name);
        hw_fmt(hw, "  if (len_%d < 0) len_%d = 0;\n", i, i);
        hw_fmt(hw, "  cur += len_%d;\n", i);
      } else if (child->kind == PEG_TOK) {
        hw_raw(hw, "  cur += 1;\n");
      }
    }
  }
  
  darray_del(all_branches);
  hw_raw(hw, "  return node;\n");
  hw_raw(hw, "}\n");
}

static void _emit_leaf_call(PegUnit* unit, IrWriter* w, const char* col_expr, char* out, int32_t out_size, char** tokens) {
  if (unit->kind == PEG_ID) {
    peg_ir_call(w, out, out_size, unit->name, "%table", col_expr);
    return;
  }
  if (unit->kind == PEG_TOK) {
    int32_t tok_id = _token_id(tokens, unit->name);
    char tok_buf[16];
    snprintf(tok_buf, sizeof(tok_buf), "%d", tok_id);
    peg_ir_tok(w, out, out_size, tok_buf, col_expr);
    return;
  }
  snprintf(out, (size_t)out_size, "-1");
}

static void _gen_unit_ir(PegUnit* unit, IrWriter* w, const char* col_expr, const char* on_success, const char* on_fail,
                         int32_t* label_counter, char* len_out, int32_t len_out_size, char** tokens) {
  if (unit->kind == PEG_TOK || unit->kind == PEG_ID) {
    if (unit->multiplier == 0) {
      char call_result[32];
      _emit_leaf_call(unit, w, col_expr, call_result, sizeof(call_result), tokens);
      char call_cmp[32];
      irwriter_icmp_imm(w, call_cmp, sizeof(call_cmp), "slt", "i32", call_result, 0);
      irwriter_br_cond(w, call_cmp, on_fail, on_success);
      snprintf(len_out, (size_t)len_out_size, "%s", call_result);
      return;
    }

    if (unit->multiplier == '?') {
      char call_result[32];
      _emit_leaf_call(unit, w, col_expr, call_result, sizeof(call_result), tokens);
      char call_cmp[32];
      irwriter_icmp_imm(w, call_cmp, sizeof(call_cmp), "slt", "i32", call_result, 0);
      char zero_or_len[32];
      peg_ir_select(w, zero_or_len, sizeof(zero_or_len), call_cmp, "i32", "0", call_result);
      snprintf(len_out, (size_t)len_out_size, "%s", zero_or_len);
      irwriter_br(w, on_success);
      return;
    }

    char total_len_ptr[32];
    peg_ir_alloca(w, total_len_ptr, sizeof(total_len_ptr), "i32");
    peg_ir_store(w, "i32", "0", total_len_ptr);

    char loop_head[32];
    char loop_body[32];
    char loop_end[32];
    snprintf(loop_head, sizeof(loop_head), "loop_head_%d", (*label_counter)++);
    snprintf(loop_body, sizeof(loop_body), "loop_body_%d", (*label_counter)++);
    snprintf(loop_end, sizeof(loop_end), "loop_end_%d", (*label_counter)++);

    if (unit->multiplier == '+') {
      char first_ok[32];
      snprintf(first_ok, sizeof(first_ok), "loop_first_ok_%d", (*label_counter)++);
      char first_call[32];
      _emit_leaf_call(unit, w, col_expr, first_call, sizeof(first_call), tokens);
      char first_cmp[32];
      irwriter_icmp_imm(w, first_cmp, sizeof(first_cmp), "slt", "i32", first_call, 0);
      irwriter_br_cond(w, first_cmp, on_fail, first_ok);

      irwriter_bb(w, first_ok);
      peg_ir_store(w, "i32", first_call, total_len_ptr);
      irwriter_br(w, loop_head);
    } else {
      irwriter_br(w, loop_head);
    }

    irwriter_bb(w, loop_head);
    char curr_total[32];
    peg_ir_load(w, curr_total, sizeof(curr_total), "i32", total_len_ptr);
    char next_col[32];
    irwriter_binop(w, next_col, sizeof(next_col), "add", "i32", col_expr, curr_total);
    char next_call[32];
    _emit_leaf_call(unit, w, next_col, next_call, sizeof(next_call), tokens);
    char next_cmp[32];
    irwriter_icmp_imm(w, next_cmp, sizeof(next_cmp), "slt", "i32", next_call, 0);
    irwriter_br_cond(w, next_cmp, loop_end, loop_body);

    irwriter_bb(w, loop_body);
    char loop_prev[32];
    peg_ir_load(w, loop_prev, sizeof(loop_prev), "i32", total_len_ptr);
    char loop_new[32];
    peg_ir_add(w, loop_new, sizeof(loop_new), loop_prev, next_call);
    peg_ir_store(w, "i32", loop_new, total_len_ptr);
    irwriter_br(w, loop_head);

    irwriter_bb(w, loop_end);
    char final_len[32];
    peg_ir_load(w, final_len, sizeof(final_len), "i32", total_len_ptr);
    snprintf(len_out, (size_t)len_out_size, "%s", final_len);
    irwriter_br(w, on_success);
  } else if (unit->kind == PEG_SEQ) {
    int32_t n = (int32_t)darray_size(unit->children);
    if (n == 0) {
      snprintf(len_out, (size_t)len_out_size, "0");
      irwriter_br(w, on_success);
      return;
    }
    char total_len[32];
    peg_ir_alloca(w, total_len, sizeof(total_len), "i32");
    peg_ir_store(w, "i32", "0", total_len);
    
    for (int32_t i = 0; i < n; i++) {
      char child_len[32];
      char prev_total[32];
      peg_ir_load(w, prev_total, sizeof(prev_total), "i32", total_len);
      char child_col[32];
      irwriter_binop(w, child_col, sizeof(child_col), "add", "i32", col_expr, prev_total);
      if (i < n - 1) {
        char next_label[32];
        snprintf(next_label, sizeof(next_label), "seq_%d", (*label_counter)++);
        _gen_unit_ir(&unit->children[i], w, child_col, next_label, on_fail, label_counter, child_len,
                     sizeof(child_len), tokens);
        irwriter_bb(w, next_label);
      } else {
        char final_label[32];
        snprintf(final_label, sizeof(final_label), "seq_final_%d", (*label_counter)++);
        _gen_unit_ir(&unit->children[i], w, child_col, final_label, on_fail, label_counter, child_len,
                     sizeof(child_len), tokens);
        irwriter_bb(w, final_label);
      }
      char new_total[32];
      peg_ir_add(w, new_total, sizeof(new_total), prev_total, child_len);
      peg_ir_store(w, "i32", new_total, total_len);
    }
    char final_len[32];
    peg_ir_load(w, final_len, sizeof(final_len), "i32", total_len);
    snprintf(len_out, (size_t)len_out_size, "%s", final_len);
    irwriter_br(w, on_success);
  } else if (unit->kind == PEG_BRANCHES) {
    int32_t n = (int32_t)darray_size(unit->children);
    for (int32_t i = 0; i < n; i++) {
      if (i < n - 1) {
        char next_branch[32];
        snprintf(next_branch, sizeof(next_branch), "try_branch_%d", (*label_counter));
        _gen_unit_ir(&unit->children[i], w, col_expr, on_success, next_branch, label_counter, len_out, len_out_size,
                     tokens);
        irwriter_bb(w, next_branch);
        (*label_counter)++;
      } else {
        _gen_unit_ir(&unit->children[i], w, col_expr, on_success, on_fail, label_counter, len_out, len_out_size,
                     tokens);
      }
    }
  }
}

static void _gen_rule_ir_naive(PegRule* rule, int32_t rule_idx, IrWriter* w, char** tokens) {
  const char* args[] = {"i8*", "i32"};
  const char* arg_names[] = {"table", "col"};
  
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "parse_%s", rule->name);
  
  irwriter_define_start(w, func_name, "i32", 2, args, arg_names);
  irwriter_bb(w, "entry");
  
  char slot_val[32];
  peg_ir_load_slot(w, slot_val, sizeof(slot_val), "%table", "%col", rule_idx);
  
  char cmp[32];
  irwriter_icmp_imm(w, cmp, sizeof(cmp), "sge", "i32", slot_val, 0);
  
  irwriter_br_cond(w, cmp, "cached", "compute");
  
  irwriter_bb(w, "cached");
  irwriter_ret(w, "i32", slot_val);
  
  irwriter_bb(w, "compute");
  
  int32_t label_counter = 0;
  char match_len[32];
  _gen_unit_ir(&rule->seq, w, "%col", "match", "fail", &label_counter, match_len, sizeof(match_len), tokens);
  
  irwriter_bb(w, "match");
  char store_buf[32];
  peg_ir_store_slot(w, store_buf, sizeof(store_buf), "%table", "%col", rule_idx, match_len);
  irwriter_ret(w, "i32", match_len);
  
  irwriter_bb(w, "fail");
  char fail_store[32];
  peg_ir_store_slot(w, fail_store, sizeof(fail_store), "%table", "%col", rule_idx, "-1");
  irwriter_ret(w, "i32", "-1");
  
  irwriter_define_end(w);
}

static void _gen_rule_ir_shared(PegRule* rule, RuleSet* rs, IrWriter* w, char** tokens) {
  const char* args[] = {"i8*", "i32"};
  const char* arg_names[] = {"table", "col"};
  
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "parse_%s", rule->name);
  
  irwriter_define_start(w, func_name, "i32", 2, args, arg_names);
  irwriter_bb(w, "entry");
  
  char bits[32];
  peg_ir_load_bits(w, bits, sizeof(bits), "%table", "%col", rs->sg_id);
  
  char rule_bit[32];
  irwriter_binop_imm(w, rule_bit, sizeof(rule_bit), "and", "i32", bits, rs->seg_mask);
  
  char cmp[32];
  irwriter_icmp_imm(w, cmp, sizeof(cmp), "eq", "i32", rule_bit, 0);
  
  irwriter_br_cond(w, cmp, "fail", "check_slot");
  
  irwriter_bb(w, "check_slot");
  char slot_val[32];
  peg_ir_load_slot(w, slot_val, sizeof(slot_val), "%table", "%col", rs->slot_idx);
  
  char slot_cmp[32];
  irwriter_icmp_imm(w, slot_cmp, sizeof(slot_cmp), "sge", "i32", slot_val, 0);
  irwriter_br_cond(w, slot_cmp, "cached", "compute");
  
  irwriter_bb(w, "cached");
  irwriter_ret(w, "i32", slot_val);
  
  irwriter_bb(w, "compute");
  
  int32_t label_counter = 0;
  char match_len[32];
  _gen_unit_ir(&rule->seq, w, "%col", "match_success", "match_fail", &label_counter, match_len,
               sizeof(match_len), tokens);
  
  irwriter_bb(w, "match_success");
  char bits_reload[32];
  peg_ir_load_bits(w, bits_reload, sizeof(bits_reload), "%table", "%col", rs->sg_id);
  char clear_mask[32];
  snprintf(clear_mask, sizeof(clear_mask), "%d", ~rs->seg_full_mask);
  char scope_cleared[32];
  irwriter_binop(w, scope_cleared, sizeof(scope_cleared), "and", "i32", bits_reload, clear_mask);
  char new_bits[32];
  irwriter_binop_imm(w, new_bits, sizeof(new_bits), "or", "i32", scope_cleared, rs->seg_mask);
  char store_bits_buf[32];
  peg_ir_store_bits(w, store_bits_buf, sizeof(store_bits_buf), "%table", "%col", rs->sg_id, new_bits);
  
  char store_slot_buf[32];
  peg_ir_store_slot(w, store_slot_buf, sizeof(store_slot_buf), "%table", "%col", rs->slot_idx, match_len);
  irwriter_ret(w, "i32", match_len);
  
  irwriter_bb(w, "match_fail");
  char bits_fail[32];
  peg_ir_load_bits(w, bits_fail, sizeof(bits_fail), "%table", "%col", rs->sg_id);
  char clear_mask_fail[32];
  snprintf(clear_mask_fail, sizeof(clear_mask_fail), "%d", ~rs->seg_mask);
  char clear_bit[32];
  irwriter_binop(w, clear_bit, sizeof(clear_bit), "and", "i32", bits_fail, clear_mask_fail);
  char store_fail_bits[32];
  peg_ir_store_bits(w, store_fail_bits, sizeof(store_fail_bits), "%table", "%col", rs->sg_id, clear_bit);
  irwriter_ret(w, "i32", "-1");
  
  irwriter_bb(w, "fail");
  irwriter_ret(w, "i32", "-1");
  
  irwriter_define_end(w);
}

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w) {
  PegRule* rules = input->rules;
  int32_t n_rules = (int32_t)darray_size(rules);

  if (n_rules == 0) {
    return;
  }

  char** tokens = darray_new(sizeof(char*), 0);
  ScopeInfo* scopes = _collect_scopes(rules, n_rules);

  RuleSet* rule_sets = calloc((size_t)n_rules, sizeof(RuleSet));
  for (int32_t i = 0; i < n_rules; i++) {
    rule_sets[i].first_set = bitset_new();
    rule_sets[i].last_set = bitset_new();
    rule_sets[i].rule = &rules[i];
  }
  
  for (int32_t i = 0; i < n_rules; i++) {
    Bitset* visited = bitset_new();
    _compute_first_set(&rules[i].seq, rule_sets[i].first_set, rule_sets, n_rules, visited, tokens);
    bitset_del(visited);

    visited = bitset_new();
    _compute_last_set(&rules[i].seq, rule_sets[i].last_set, rule_sets, n_rules, visited, tokens);
    bitset_del(visited);
  }

  _gen_ref_type(hw);

  for (int32_t i = 0; i < n_rules; i++) {
    _gen_node_type(hw, &rules[i]);
  }

  hw_blank(hw);
  if (input->mode == PEG_MODE_NAIVE) {
    int32_t max_scope_rules = 0;
    for (int32_t i = 0; i < (int32_t)darray_size(scopes); i++) {
      if (scopes[i].n_rules > max_scope_rules) {
        max_scope_rules = scopes[i].n_rules;
      }
      for (int32_t j = 0; j < scopes[i].n_rules; j++) {
        int32_t ri = scopes[i].rule_indices[j];
        rule_sets[ri].slot_idx = j;
      }
    }
    hw_struct_begin(hw, "Col");
    hw_fmt(hw, "  int32_t slots[%d];\n", max_scope_rules > 0 ? max_scope_rules : 1);
    hw_struct_end(hw);
    hw_raw(hw, " Col;\n\n");
  } else {
    Graph* g = _build_interference_graph(rule_sets, n_rules);
    int32_t* edges = graph_edges(g);
    int32_t n_edges = graph_n_edges(g);
    
    ColoringResult* cr = coloring_solve(n_rules, edges, n_edges, n_rules, 1000000, 42);
    int32_t sg_size = coloring_get_sg_size(cr);
    
    int32_t max_color = 0;
    for (int32_t i = 0; i < n_rules; i++) {
      coloring_get_segment_info(cr, i, &rule_sets[i].sg_id, &rule_sets[i].seg_mask);
      rule_sets[i].slot_idx = rule_sets[i].sg_id;
      if (rule_sets[i].sg_id > max_color) {
        max_color = rule_sets[i].sg_id;
      }
    }

    for (int32_t i = 0; i < n_rules; i++) {
      int32_t full = 0;
      for (int32_t j = 0; j < n_rules; j++) {
        if (rule_sets[i].sg_id == rule_sets[j].sg_id &&
            strcmp(_scope_name(rule_sets[i].rule), _scope_name(rule_sets[j].rule)) == 0) {
          full |= rule_sets[j].seg_mask;
        }
      }
      rule_sets[i].seg_full_mask = full;
    }
    
    hw_struct_begin(hw, "Col");
    hw_fmt(hw, "  int32_t bits[%d];\n", sg_size);
    hw_fmt(hw, "  int32_t slots[%d];\n", max_color + 1);
    hw_struct_end(hw);
    hw_raw(hw, " Col;\n\n");
    
    coloring_result_del(cr);
    graph_del(g);
  }

  hw_blank(hw);
  hw_raw(hw, "static inline bool peg_has_next(PegRef ref) { return ref.next_col >= 0; }\n");
  hw_raw(hw, "static inline PegRef peg_get_next(PegRef ref) { return (PegRef){ref.table, ref.next_col, -1}; }\n");
  hw_blank(hw);
  hw_raw(hw, "#include <stdlib.h>\n");
  hw_raw(hw, "#include <string.h>\n");
  hw_blank(hw);
  hw_fmt(hw, "static inline Col* peg_alloc_table(int32_t n_cols) {\n");
  hw_fmt(hw, "  Col* table = (Col*)malloc(sizeof(Col) * n_cols);\n");
  hw_fmt(hw, "  if (table) memset(table, 0xFF, sizeof(Col) * n_cols);\n");
  hw_fmt(hw, "  return table;\n");
  hw_fmt(hw, "}\n");
  hw_blank(hw);
  hw_raw(hw, "static inline void peg_free_table(Col* table) { free(table); }\n");

  for (int32_t i = 0; i < n_rules; i++) {
    _gen_load_impl(hw, &rules[i]);
  }

  irwriter_declare(w, "i32", "match_tok", "i32, i32");

  for (int32_t i = 0; i < n_rules; i++) {
    hw_fmt(hw, "int32_t parse_%s(void* table, int32_t col);\n", rules[i].name);
  }
  hw_blank(hw);

  if (input->mode == PEG_MODE_NAIVE) {
    for (int32_t i = 0; i < n_rules; i++) {
      _gen_rule_ir_naive(&rules[i], rule_sets[i].slot_idx, w, tokens);
    }
  } else {
    for (int32_t i = 0; i < n_rules; i++) {
      _gen_rule_ir_shared(&rules[i], &rule_sets[i], w, tokens);
    }
  }

  for (int32_t i = 0; i < n_rules; i++) {
    bitset_del(rule_sets[i].first_set);
    bitset_del(rule_sets[i].last_set);
  }
  for (int32_t i = 0; i < (int32_t)darray_size(tokens); i++) {
    free(tokens[i]);
  }
  darray_del(tokens);
  _free_scopes(scopes);
  free(rule_sets);
}
