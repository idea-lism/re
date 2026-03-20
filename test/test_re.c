#include "../src/re.h"
#include <assert.h>
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

#define TARGET "arm64-apple-macosx14.0.0"

static char* gen_ir(void (*fn)(Aut*, Re*, IrWriter*)) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = open_memstream(&buf, &sz);
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
  irwriter_start(w, "test.rules", ".");

  Aut* a = aut_new("match", "test.rules");
  Re* re = re_new(a);
  fn(a, re, w);
  re_del(re);
  aut_del(a);

  irwriter_end(w);
  irwriter_del(w);
  fclose(f);
  return buf;
}

// --- Lifecycle ---

TEST(test_lifecycle) {
  Aut* a = aut_new("f", "test.rules");
  Re* re = re_new(a);
  assert(re);
  re_del(re);
  aut_del(a);
}

// --- Negated ranges ---

TEST(test_neg_empty) {
  // No ranges to negate: should yield [0, 0x10FFFF]
  NegRangeIter iter;
  re_neg_ranges(&iter, 0, NULL);
  assert(!iter.done);
  assert(iter.start == 0);
  assert(iter.end == 0x10FFFF);
  re_neg_next(&iter);
  assert(iter.done);
}

TEST(test_neg_single) {
  // Negate [10, 20]: should yield [0,9] and [21, 0x10FFFF]
  Range ranges[] = {{10, 20}};
  NegRangeIter iter;
  re_neg_ranges(&iter, 1, ranges);
  assert(!iter.done);
  assert(iter.start == 0);
  assert(iter.end == 9);

  re_neg_next(&iter);
  assert(!iter.done);
  assert(iter.start == 21);
  assert(iter.end == 0x10FFFF);

  re_neg_next(&iter);
  assert(iter.done);
}

TEST(test_neg_at_start) {
  // Negate [0, 5]: should yield [6, 0x10FFFF]
  Range ranges[] = {{0, 5}};
  NegRangeIter iter;
  re_neg_ranges(&iter, 1, ranges);
  assert(!iter.done);
  assert(iter.start == 6);
  assert(iter.end == 0x10FFFF);

  re_neg_next(&iter);
  assert(iter.done);
}

TEST(test_neg_at_end) {
  // Negate [0x10FFFE, 0x10FFFF]: should yield [0, 0x10FFFD]
  Range ranges[] = {{0x10FFFE, 0x10FFFF}};
  NegRangeIter iter;
  re_neg_ranges(&iter, 1, ranges);
  assert(!iter.done);
  assert(iter.start == 0);
  assert(iter.end == 0x10FFFD);

  re_neg_next(&iter);
  assert(iter.done);
}

TEST(test_neg_full) {
  // Negate [0, 0x10FFFF]: no gaps
  Range ranges[] = {{0, 0x10FFFF}};
  NegRangeIter iter;
  re_neg_ranges(&iter, 1, ranges);
  assert(iter.done);
}

TEST(test_neg_multiple) {
  // Negate [5,10], [20,30]: gaps are [0,4], [11,19], [31, 0x10FFFF]
  Range ranges[] = {{5, 10}, {20, 30}};
  NegRangeIter iter;
  re_neg_ranges(&iter, 2, ranges);

  assert(!iter.done);
  assert(iter.start == 0);
  assert(iter.end == 4);

  re_neg_next(&iter);
  assert(!iter.done);
  assert(iter.start == 11);
  assert(iter.end == 19);

  re_neg_next(&iter);
  assert(!iter.done);
  assert(iter.start == 31);
  assert(iter.end == 0x10FFFF);

  re_neg_next(&iter);
  assert(iter.done);
}

TEST(test_neg_overlapping) {
  // Negate [5,15], [10,20]: effective [5,20], gaps [0,4], [21, 0x10FFFF]
  Range ranges[] = {{5, 15}, {10, 20}};
  NegRangeIter iter;
  re_neg_ranges(&iter, 2, ranges);

  assert(!iter.done);
  assert(iter.start == 0);
  assert(iter.end == 4);

  re_neg_next(&iter);
  assert(!iter.done);
  assert(iter.start == 21);
  assert(iter.end == 0x10FFFF);

  re_neg_next(&iter);
  assert(iter.done);
}

TEST(test_neg_adjacent) {
  // Negate [5,10], [11,20]: effective [5,20], gaps [0,4], [21, 0x10FFFF]
  Range ranges[] = {{5, 10}, {11, 20}};
  NegRangeIter iter;
  re_neg_ranges(&iter, 2, ranges);

  assert(!iter.done);
  assert(iter.start == 0);
  assert(iter.end == 4);

  re_neg_next(&iter);
  assert(!iter.done);
  assert(iter.start == 21);
  assert(iter.end == 0x10FFFF);

  re_neg_next(&iter);
  assert(iter.done);
}

TEST(test_neg_unsorted) {
  // Negate [20,30], [5,10] (unsorted): gaps [0,4], [11,19], [31, 0x10FFFF]
  Range ranges[] = {{20, 30}, {5, 10}};
  NegRangeIter iter;
  re_neg_ranges(&iter, 2, ranges);

  assert(!iter.done);
  assert(iter.start == 0);
  assert(iter.end == 4);

  re_neg_next(&iter);
  assert(!iter.done);
  assert(iter.start == 11);
  assert(iter.end == 19);

  re_neg_next(&iter);
  assert(!iter.done);
  assert(iter.start == 31);
  assert(iter.end == 0x10FFFF);

  re_neg_next(&iter);
  assert(iter.done);
}

// --- re_range ---

static void build_range(Aut* a, Re* re, IrWriter* w) {
  re_range(re, 'A', 'Z');
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_range) {
  char* out = gen_ir(build_range);
  // Should have range check for A-Z (65-90)
  assert(strstr(out, "icmp sge i32 %cp, 65"));
  assert(strstr(out, "icmp sle i32 %cp, 90"));
  free(out);
}

// --- re_seq ---

static void build_seq(Aut* a, Re* re, IrWriter* w) {
  re_seq(re, (int32_t)'H', (int32_t)'i', (int32_t)-1);
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_seq) {
  char* out = gen_ir(build_seq);
  // 'H'=72, 'i'=105 should both appear
  assert(strstr(out, "i32 72"));
  assert(strstr(out, "i32 105"));
  free(out);
}

// --- re_lparen / re_rparen (single branch, grouping) ---

static void build_group(Aut* a, Re* re, IrWriter* w) {
  // (ab)c with action 1
  re_lparen(re);
  re_seq(re, (int32_t)'a', (int32_t)'b', (int32_t)-1);
  re_rparen(re);
  re_seq(re, (int32_t)'c', (int32_t)-1);
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_group) {
  char* out = gen_ir(build_group);
  // 'a'=97, 'b'=98, 'c'=99
  assert(strstr(out, "i32 97"));
  assert(strstr(out, "i32 98"));
  assert(strstr(out, "i32 99"));
  free(out);
}

// --- re_fork (alternation) ---

static void build_alt(Aut* a, Re* re, IrWriter* w) {
  // (a|b) with action 1
  re_lparen(re);
  re_seq(re, (int32_t)'a', (int32_t)-1);
  re_fork(re);
  re_seq(re, (int32_t)'b', (int32_t)-1);
  re_rparen(re);
  re_action(re, 1);
  aut_gen_dfa(a, w, false);
}

TEST(test_alt) {
  char* out = gen_ir(build_alt);
  // Both 'a'=97 and 'b'=98 should reach a state with action 1
  assert(strstr(out, "i32 97"));
  assert(strstr(out, "i32 98"));
  free(out);
}

// --- Complex: (ab|cd)e ---

static void build_complex(Aut* a, Re* re, IrWriter* w) {
  re_lparen(re);
  re_seq(re, (int32_t)'a', (int32_t)'b', (int32_t)-1);
  re_fork(re);
  re_seq(re, (int32_t)'c', (int32_t)'d', (int32_t)-1);
  re_rparen(re);
  re_seq(re, (int32_t)'e', (int32_t)-1);
  re_action(re, 1);
  aut_optimize(a);
  aut_gen_dfa(a, w, false);
}

TEST(test_complex) {
  char* out = gen_ir(build_complex);
  assert(strstr(out, "define {i32, i32} @match"));
  // all chars present
  assert(strstr(out, "i32 97"));  // a
  assert(strstr(out, "i32 98"));  // b
  assert(strstr(out, "i32 99"));  // c
  assert(strstr(out, "i32 100")); // d
  assert(strstr(out, "i32 101")); // e
  free(out);
}

// --- re_action ---

static void build_action(Aut* a, Re* re, IrWriter* w) {
  re_range(re, 'x', 'x');
  re_action(re, 42);
  aut_gen_dfa(a, w, false);
}

TEST(test_action) {
  char* out = gen_ir(build_action);
  assert(strstr(out, "i32 42, 1")); // action_id=42 in insertvalue
  free(out);
}

// --- Clang compilation ---

static void write_and_compile(void (*fn)(Aut*, Re*, IrWriter*), const char* test_name) {
  char ll_path[128], obj_path[128];
  snprintf(ll_path, sizeof(ll_path), "/tmp/test_re_%s.ll", test_name);
  snprintf(obj_path, sizeof(obj_path), "/tmp/test_re_%s.o", test_name);

  FILE* f = fopen(ll_path, "w");
  assert(f);
  IrWriter* w = irwriter_new(f, TARGET);
  irwriter_start(w, "test.rules", ".");

  Aut* a = aut_new("match", "test.rules");
  Re* re = re_new(a);
  fn(a, re, w);
  re_del(re);
  aut_del(a);

  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "xcrun clang -c %s -o %s 2>&1", ll_path, obj_path);
  FILE* p = popen(cmd, "r");
  assert(p);
  char output[4096] = {0};
  size_t n = fread(output, 1, sizeof(output) - 1, p);
  output[n] = '\0';
  int32_t status = pclose(p);
  if (status != 0) {
    fprintf(stderr, "\nclang failed for %s:\n%s\n", test_name, output);
    FILE* ll = fopen(ll_path, "r");
    if (ll) {
      char line[512];
      while (fgets(line, sizeof(line), ll)) {
        fputs(line, stderr);
      }
      fclose(ll);
    }
  }
  assert(status == 0);
  remove(obj_path);
  remove(ll_path);
}

TEST(test_compile_range) { write_and_compile(build_range, "range"); }
TEST(test_compile_seq) { write_and_compile(build_seq, "seq"); }
TEST(test_compile_group) { write_and_compile(build_group, "group"); }
TEST(test_compile_alt) { write_and_compile(build_alt, "alt"); }
TEST(test_compile_complex) { write_and_compile(build_complex, "complex"); }
TEST(test_compile_action) { write_and_compile(build_action, "action"); }

int main(void) {
  printf("test_re:\n");
  RUN(test_lifecycle);
  RUN(test_neg_empty);
  RUN(test_neg_single);
  RUN(test_neg_at_start);
  RUN(test_neg_at_end);
  RUN(test_neg_full);
  RUN(test_neg_multiple);
  RUN(test_neg_overlapping);
  RUN(test_neg_adjacent);
  RUN(test_neg_unsorted);
  RUN(test_range);
  RUN(test_seq);
  RUN(test_group);
  RUN(test_alt);
  RUN(test_complex);
  RUN(test_action);
  RUN(test_compile_range);
  RUN(test_compile_seq);
  RUN(test_compile_group);
  RUN(test_compile_alt);
  RUN(test_compile_complex);
  RUN(test_compile_action);
  printf("all ok\n");
  return 0;
}
