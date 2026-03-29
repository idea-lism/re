#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/parse.h"
#include "compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

void parse_nest(const char* src, HeaderWriter* header_writer, IrWriter* ir_writer);

// Minimal valid test source: one scope with tokens that match PEG usage.
// main scope emits @id and @assign (ignoring @comment and @space).
// PEG main rule uses exactly @id and @assign.
static const char* TEST_SRC = "[[vpa]]\n"
                              "\n"
                              "%ignore @comment @space\n"
                              "\n"
                              "%keyword ops \"=\" \"|\"\n"
                              "\n"
                              "main = {\n"
                              "  /[a-z_]\\w*/ @id\n"
                              "  /=/ @assign\n"
                              "  /#[^\\n]*/ @comment\n"
                              "  /[ \\t\\n]+/ @space\n"
                              "}\n"
                              "\n"
                              "[[peg]]\n"
                              "\n"
                              "main = @id @assign @id\n"
                              "\n";

// Extended test source with scopes, leader inlining, hooks, keywords, and macros.
static const char* TEST_SRC_SCOPED = "[[vpa]]\n"
                                     "\n"
                                     "%ignore @comment @space\n"
                                     "\n"
                                     "%keyword ops \"=\" \"|\"\n"
                                     "\n"
                                     "main = {\n"
                                     "  inner\n"
                                     "  /\\n+/ @nl\n"
                                     "  /#[^\\n]*/ @comment\n"
                                     "  /[ \\t\\n]+/ @space\n"
                                     "}\n"
                                     "\n"
                                     "inner = /\\{/ .begin {\n"
                                     "  /\\}/ .end\n"
                                     "  /[a-z_]\\w*/ @id\n"
                                     "  /=/ @assign\n"
                                     "  /#[^\\n]*/ @comment\n"
                                     "  /[ \\t\\n]+/ @space\n"
                                     "}\n"
                                     "\n"
                                     "[[peg]]\n"
                                     "\n"
                                     "main = @nl* inner+<@nl> @nl*\n"
                                     "\n"
                                     "inner = @id @assign @id\n"
                                     "\n";

static void _gen_output(const char* src, char** hdr_out, size_t* hdr_sz_out, char** ir_out, size_t* ir_sz_out) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_sz);
  assert(hdr_f);

  char* ir_buf = NULL;
  size_t ir_sz = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_sz);
  assert(ir_f);

  HeaderWriter* hw = hw_new(hdr_f);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.nest", ".");

  parse_nest(src, hw, w);

  irwriter_end(w);
  irwriter_del(w);
  hw_del(hw);

  compat_close_memstream(hdr_f, &hdr_buf, &hdr_sz);
  compat_close_memstream(ir_f, &ir_buf, &ir_sz);

  *hdr_out = hdr_buf;
  *hdr_sz_out = hdr_sz;
  *ir_out = ir_buf;
  *ir_sz_out = ir_sz;
}

static bool _gen_succeeds(const char* src) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  bool ok = hdr_sz > 0 && ir_sz > 0;
  free(hdr_buf);
  free(ir_buf);
  return ok;
}

TEST(test_parse_basic) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  assert(hdr_buf != NULL);
  assert(hdr_sz > 0);
  assert(ir_buf != NULL);
  assert(ir_sz > 0);

  free(hdr_buf);
  free(ir_buf);
}

TEST(test_scoped) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC_SCOPED, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  assert(hdr_buf != NULL);
  assert(hdr_sz > 0);
  assert(ir_buf != NULL);
  assert(ir_sz > 0);

  // DFA functions for scopes
  assert(strstr(ir_buf, "@lex_main("));
  assert(strstr(ir_buf, "@lex_inner("));

  // Scope IDs
  assert(strstr(hdr_buf, "SCOPE_MAIN"));
  assert(strstr(hdr_buf, "SCOPE_INNER"));

  // Dispatch and lex loop
  assert(strstr(ir_buf, "@vpa_dispatch("));
  assert(strstr(ir_buf, "@vpa_lex("));
  assert(strstr(ir_buf, "call void @vpa_rt_push_scope"));
  assert(strstr(ir_buf, "call void @vpa_rt_pop_scope"));
  assert(strstr(ir_buf, "call void @vpa_rt_emit_token"));

  // Token IDs
  assert(strstr(hdr_buf, "TOK_ID"));
  assert(strstr(hdr_buf, "TOK_ASSIGN"));
  assert(strstr(hdr_buf, "TOK_NL"));

  free(hdr_buf);
  free(ir_buf);
}

TEST(test_vpa_ir_compiles) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC_SCOPED, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  // Truncate before PEG functions (PEG may have forward refs that fail standalone)
  char* peg_start = strstr(ir_buf, "define i32 @parse_");
  if (peg_start) {
    *peg_start = '\0';
  }

  char ll_path[256];
  char obj_path[256];
  snprintf(ll_path, sizeof(ll_path), "%s/test_vpa_compile.ll", BUILD_DIR);
  snprintf(obj_path, sizeof(obj_path), "%s/test_vpa_compile.o", BUILD_DIR);
  FILE* f = fopen(ll_path, "w");
  assert(f);
  fputs(ir_buf, f);
  fclose(f);

  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s -c -o %s %s 2>&1", compat_llvm_cc(), obj_path, ll_path);
  int rc = system(cmd);
  if (rc != 0) {
    fprintf(stderr, "clang compilation failed for %s\n", ll_path);
  }
  assert(rc == 0);

  remove(obj_path);
  remove(ll_path);
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_generated_runtime_header_compiles) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  char hdr_path[256];
  char c_path[256];
  char obj_path[256];
  snprintf(hdr_path, sizeof(hdr_path), "%s/test_generated_runtime.h", BUILD_DIR);
  snprintf(c_path, sizeof(c_path), "%s/test_generated_runtime.c", BUILD_DIR);
  snprintf(obj_path, sizeof(obj_path), "%s/test_generated_runtime.o", BUILD_DIR);

  FILE* hf = fopen(hdr_path, "w");
  assert(hf);
  fputs(hdr_buf, hf);
  fclose(hf);

  FILE* cf = fopen(c_path, "w");
  assert(cf);
  fprintf(cf, "#define VPA_IMPLEMENTATION\n");
  fprintf(cf, "#include \"test_generated_runtime.h\"\n");
  fprintf(cf, "int32_t vpa_rt_read_cp(void* src, int32_t cp_off) { (void)src; (void)cp_off; return -2; }\n");
  fprintf(cf, "int main(void) { return 0; }\n");
  fclose(cf);

  char cmd[768];
  snprintf(cmd, sizeof(cmd), "%s -c -o %s %s 2>&1", compat_llvm_cc(), obj_path, c_path);
  int rc = system(cmd);
  if (rc != 0) {
    fprintf(stderr, "clang compilation failed for %s\n", c_path);
  }
  assert(rc == 0);

  remove(obj_path);
  remove(c_path);
  remove(hdr_path);
  free(hdr_buf);
  free(ir_buf);
}

// --- Token set validation tests ---

TEST(test_validate_peg_uses_missing_token) {
  // PEG uses @missing which VPA doesn't emit
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a @tok_b\n";
  assert(!_gen_succeeds(src));
}

TEST(test_validate_vpa_emits_unused_token) {
  // VPA emits @tok_b which PEG doesn't use
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  /y/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(!_gen_succeeds(src));
}

TEST(test_validate_matching_sets) {
  // VPA emit set == PEG used set
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  /y/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a @tok_b\n";
  assert(_gen_succeeds(src));
}

TEST(test_validate_ignored_tokens_excluded) {
  // @space is ignored so not required in PEG
  const char* src = "[[vpa]]\n"
                    "%ignore @space\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  / +/ @space\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(_gen_succeeds(src));
}

TEST(test_validate_peg_subrule_tokens) {
  // PEG sub-rule uses @tok_b — still counts in main scope's used set
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  /y/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a sub\n"
                    "sub = @tok_b\n";
  assert(_gen_succeeds(src));
}

TEST(test_validate_scope_tokens_not_mixed) {
  // @inner_tok is emitted in inner scope, not in main
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  inner\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "inner = /\\{/ .begin {\n"
                    "  /\\}/ .end\n"
                    "  /y/ @inner_tok\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a inner\n"
                    "inner = @inner_tok\n";
  assert(_gen_succeeds(src));
}

// --- Phase 1: Validation error paths ---

TEST(test_err_missing_main) {
  const char* src = "[[vpa]]\n"
                    "foo = {\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "[[peg]]\n"
                    "foo = @tok_a\n";
  assert(!_gen_succeeds(src));
}

TEST(test_err_scope_missing_end) {
  // inner is a scope rule (= { ... }) with .begin but no .end
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  inner\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "inner = {\n"
                    "  /\\{/ .begin\n"
                    "  /y/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a inner\n"
                    "inner = @tok_b\n";
  assert(!_gen_succeeds(src));
}

TEST(test_err_undeclared_state) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  $undeclared\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(!_gen_succeeds(src));
}

TEST(test_err_empty_input) { assert(!_gen_succeeds("")); }

TEST(test_err_no_vpa_section) {
  const char* src = "[[peg]]\n"
                    "main = @a\n";
  assert(!_gen_succeeds(src));
}

TEST(test_err_trailing_tokens) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n"
                    "[[vpa]]\n";
  assert(!_gen_succeeds(src));
}

// --- Phase 2: VPA parsing features ---

TEST(test_vpa_str_span) {
  const char* src = "[[vpa]]\n"
                    "%ignore @space\n"
                    "main = {\n"
                    "  \"hello\" @tok_hello\n"
                    "  /[ \\t\\n]+/ @space\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_hello\n";
  assert(_gen_succeeds(src));
}

TEST(test_vpa_state_ref) {
  const char* src = "[[vpa]]\n"
                    "%state $mode\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  $mode\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  assert(hdr_sz > 0 && ir_sz > 0);
  // state generates a matcher function or declaration
  assert(strstr(ir_buf, "mode") || strstr(hdr_buf, "mode"));
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_vpa_pipe) {
  const char* src = "[[vpa]]\n"
                    "%ignore @space\n"
                    "main = {\n"
                    "  /x/ @tok_a | /y/ @tok_b\n"
                    "  /[ \\t\\n]+/ @space\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a @tok_b\n";
  assert(_gen_succeeds(src));
}

TEST(test_vpa_macro) {
  const char* src = "[[vpa]]\n"
                    "%ignore @space\n"
                    "*ws = {\n"
                    "  /[ \\t\\n]+/ @space\n"
                    "}\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  *ws\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(_gen_succeeds(src));
}

TEST(test_vpa_user_hook) {
  const char* src = "[[vpa]]\n"
                    "%effect .my_hook = .begin\n"
                    "main = {\n"
                    "  inner\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "inner = /\\{/ .my_hook {\n"
                    "  /\\}/ .end\n"
                    "  /y/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a inner\n"
                    "inner = @tok_b\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  assert(hdr_sz > 0 && ir_sz > 0);
  // user hook callback referenced in IR
  assert(strstr(ir_buf, "my_hook"));
  // scope IDs for both main and inner
  assert(strstr(hdr_buf, "SCOPE_MAIN"));
  assert(strstr(hdr_buf, "SCOPE_INNER"));
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_vpa_fail_hook) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  /invalid/ .fail\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(_gen_succeeds(src));
}

TEST(test_vpa_unparse_hook) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  inner\n"
                    "  /x/ @tok_a\n"
                    "}\n"
                    "inner = /\\{/ .begin {\n"
                    "  /\\}/ .unparse .end\n"
                    "  /y/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a inner\n"
                    "inner = @tok_b\n";
  assert(_gen_succeeds(src));
}

// --- Phase 3: PEG parsing features ---

TEST(test_peg_branches) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /a/ @tok_a\n"
                    "  /b/ @tok_b\n"
                    "  /c/ @tok_c\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = [\n"
                    "  branch_a: @tok_a\n"
                    "  branch_b: @tok_b\n"
                    "  branch_c: @tok_c\n"
                    "]\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  assert(hdr_sz > 0 && ir_sz > 0);
  // branch tag bitfields
  assert(strstr(hdr_buf, "bool branch_a : 1"));
  assert(strstr(hdr_buf, "bool branch_b : 1"));
  assert(strstr(hdr_buf, "bool branch_c : 1"));
  // load function extracts branch_id
  assert(strstr(hdr_buf, "branch_id"));
  assert(strstr(hdr_buf, "node.is.branch_a"));
  assert(strstr(hdr_buf, "node.is.branch_b"));
  assert(strstr(hdr_buf, "node.is.branch_c"));
  // IR has backtracking
  assert(strstr(ir_buf, "backtrack_push"));
  assert(strstr(ir_buf, "backtrack_restore"));
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_peg_multipliers) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /a/ @tok_a\n"
                    "  /b/ @tok_b\n"
                    "  /c/ @tok_c\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a? @tok_b+ @tok_c*\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  assert(hdr_sz > 0 && ir_sz > 0);
  // node struct with next_col for + and * children
  assert(strstr(hdr_buf, "MainNode"));
  assert(strstr(hdr_buf, "next_col"));
  // IR: parse function with match_tok calls
  assert(strstr(ir_buf, "define i32 @parse_main"));
  assert(strstr(ir_buf, "@match_tok"));
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_peg_interlace) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /a/ @tok_a\n"
                    "  /,/ @tok_comma\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a+<@tok_comma>\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  assert(hdr_sz > 0 && ir_sz > 0);
  assert(strstr(hdr_buf, "MainNode"));
  assert(strstr(hdr_buf, "next_col"));
  // IR: interlace loop has two match_tok calls (separator + element)
  assert(strstr(ir_buf, "define i32 @parse_main"));
  // at least 2 match_tok calls for interlace pattern
  char* first = strstr(ir_buf, "@match_tok");
  assert(first);
  assert(strstr(first + 1, "@match_tok"));
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_peg_tags) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /a/ @tok_a\n"
                    "  /b/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = [\n"
                    "  first: @tok_a\n"
                    "  second: @tok_b\n"
                    "]\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  assert(hdr_sz > 0 && ir_sz > 0);
  assert(strstr(hdr_buf, "bool first : 1"));
  assert(strstr(hdr_buf, "bool second : 1"));
  assert(strstr(hdr_buf, "node.is.first"));
  assert(strstr(hdr_buf, "node.is.second"));
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_peg_subrule_ref) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /a/ @tok_a\n"
                    "  /b/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = item+\n"
                    "item = @tok_a @tok_b\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  assert(hdr_sz > 0 && ir_sz > 0);
  // both rules get node types and parse functions
  assert(strstr(hdr_buf, "MainNode"));
  assert(strstr(hdr_buf, "ItemNode"));
  assert(strstr(ir_buf, "define i32 @parse_main"));
  assert(strstr(ir_buf, "define i32 @parse_item"));
  // main calls item
  assert(strstr(ir_buf, "@parse_item("));
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_peg_auto_tag) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /a/ @tok_a\n"
                    "  /b/ @tok_b\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = [\n"
                    "  @tok_a\n"
                    "  @tok_b\n"
                    "]\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  assert(hdr_sz > 0 && ir_sz > 0);
  // auto-tagging names branches after first child token
  assert(strstr(hdr_buf, "bool tok_a : 1"));
  assert(strstr(hdr_buf, "bool tok_b : 1"));
  assert(strstr(hdr_buf, "branch_id"));
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_peg_cross_bracket_dup_tag) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /a/ @tok_a\n"
                    "  /b/ @tok_b\n"
                    "  /c/ @tok_c\n"
                    "  /d/ @tok_d\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = [\n"
                    "  dup: @tok_a\n"
                    "  other: @tok_b\n"
                    "] [\n"
                    "  dup: @tok_c\n"
                    "  another: @tok_d\n"
                    "]\n";
  assert(!_gen_succeeds(src));
}

// --- Phase 4: Post-processing ---

TEST(test_keyword_expansion) {
  // %keyword: group name matches a rule, expansion adds regexp units to it
  const char* src = "[[vpa]]\n"
                    "%ignore @space\n"
                    "%keyword kw \"if\" \"else\"\n"
                    "main = {\n"
                    "  kw\n"
                    "  /[a-z]+/ @id\n"
                    "  /[ \\t\\n]+/ @space\n"
                    "}\n"
                    "kw = {\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @if @id @else @id\n";
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(src, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);
  assert(hdr_sz > 0 && ir_sz > 0);
  // keyword expansion produces token defs with group.literal naming
  assert(strstr(hdr_buf, "KW_IF") || strstr(hdr_buf, "kw.if"));
  assert(strstr(hdr_buf, "KW_ELSE") || strstr(hdr_buf, "kw.else"));
  free(hdr_buf);
  free(ir_buf);
}

TEST(test_keyword_multiple_groups) {
  const char* src = "[[vpa]]\n"
                    "%ignore @space\n"
                    "%keyword kw \"if\"\n"
                    "%keyword ty \"int\"\n"
                    "main = {\n"
                    "  kw\n"
                    "  ty\n"
                    "  /[a-z]+/ @id\n"
                    "  /[ \\t\\n]+/ @space\n"
                    "}\n"
                    "kw = {\n"
                    "}\n"
                    "ty = {\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @if @int @id\n";
  assert(_gen_succeeds(src));
}

TEST(test_macro_inline_nested) {
  const char* src = "[[vpa]]\n"
                    "%ignore @space @nl\n"
                    "*ws = {\n"
                    "  /[ \\t]+/ @space\n"
                    "  /\\n+/ @nl\n"
                    "}\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  *ws\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(_gen_succeeds(src));
}

TEST(test_macro_undefined) {
  // undefined macro ref remains as a unit, causing token set mismatch
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /x/ @tok_a\n"
                    "  *nonexistent\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(!_gen_succeeds(src));
}

// --- Phase 5: DFA lexer edge cases ---

TEST(test_lex_invalid_utf8) {
  const char* src = "[[vpa]]\n\x80\x81\n";
  assert(!_gen_succeeds(src));
}

TEST(test_lex_single_quote_str) {
  const char* src = "[[vpa]]\n"
                    "%ignore @space\n"
                    "main = {\n"
                    "  'hello' @tok_hello\n"
                    "  /[ \\t\\n]+/ @space\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_hello\n";
  assert(_gen_succeeds(src));
}

TEST(test_lex_negated_charclass) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /[^abc]+/ @tok_a\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(_gen_succeeds(src));
}

TEST(test_lex_charclass_escape) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /[a-z\\d]+/ @tok_a\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(_gen_succeeds(src));
}

TEST(test_lex_unicode_escape) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /\\u{0041}/ @tok_a\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(_gen_succeeds(src));
}

TEST(test_lex_bof_eof) {
  const char* src = "[[vpa]]\n"
                    "main = {\n"
                    "  /\\a[a-z]+\\z/ @tok_a\n"
                    "}\n"
                    "[[peg]]\n"
                    "main = @tok_a\n";
  assert(_gen_succeeds(src));
}

int main(void) {
  printf("test_parse:\n");

  RUN(test_parse_basic);
  RUN(test_scoped);
  RUN(test_vpa_ir_compiles);
  RUN(test_generated_runtime_header_compiles);
  RUN(test_validate_peg_uses_missing_token);
  RUN(test_validate_vpa_emits_unused_token);
  RUN(test_validate_matching_sets);
  RUN(test_validate_ignored_tokens_excluded);
  RUN(test_validate_peg_subrule_tokens);
  RUN(test_validate_scope_tokens_not_mixed);

  // Phase 1: Validation error paths
  RUN(test_err_missing_main);
  RUN(test_err_scope_missing_end);
  RUN(test_err_undeclared_state);
  RUN(test_err_empty_input);
  RUN(test_err_no_vpa_section);
  RUN(test_err_trailing_tokens);

  // Phase 2: VPA parsing features
  RUN(test_vpa_str_span);
  RUN(test_vpa_state_ref);
  RUN(test_vpa_pipe);
  RUN(test_vpa_macro);
  RUN(test_vpa_user_hook);
  RUN(test_vpa_fail_hook);
  RUN(test_vpa_unparse_hook);

  // Phase 3: PEG parsing features
  RUN(test_peg_branches);
  RUN(test_peg_multipliers);
  RUN(test_peg_interlace);
  RUN(test_peg_tags);
  RUN(test_peg_subrule_ref);
  RUN(test_peg_auto_tag);
  RUN(test_peg_cross_bracket_dup_tag);

  // Phase 4: Post-processing
  RUN(test_keyword_expansion);
  RUN(test_keyword_multiple_groups);
  RUN(test_macro_inline_nested);
  RUN(test_macro_undefined);

  // Phase 5: DFA lexer edge cases
  RUN(test_lex_invalid_utf8);
  RUN(test_lex_single_quote_str);
  RUN(test_lex_negated_charclass);
  RUN(test_lex_charclass_escape);
  RUN(test_lex_unicode_escape);
  RUN(test_lex_bof_eof);

  printf("all ok\n");
  return 0;
}
