#include "../src/bitset.h"
#include <assert.h>
#include <stdio.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// --- Basic ---

TEST(test_new_del) {
  Bitset* bs = bitset_new();
  assert(bs != NULL);
  assert(bitset_size(bs) == 0);
  bitset_del(bs);
}

TEST(test_add_contains) {
  Bitset* bs = bitset_new();
  assert(!bitset_contains(bs, 0));
  bitset_add_bit(bs, 0);
  assert(bitset_contains(bs, 0));
  assert(!bitset_contains(bs, 1));
  bitset_add_bit(bs, 63);
  assert(bitset_contains(bs, 63));
  assert(bitset_size(bs) == 2);
  bitset_del(bs);
}

TEST(test_clear_bit) {
  Bitset* bs = bitset_new();
  bitset_add_bit(bs, 10);
  assert(bitset_contains(bs, 10));
  bitset_clear_bit(bs, 10);
  assert(!bitset_contains(bs, 10));
  assert(bitset_size(bs) == 0);
  bitset_del(bs);
}

TEST(test_clear_empty) {
  Bitset* bs = bitset_new();
  bitset_clear_bit(bs, 999);
  assert(bitset_size(bs) == 0);
  bitset_del(bs);
}

TEST(test_contains_empty) {
  Bitset* bs = bitset_new();
  assert(!bitset_contains(bs, 0));
  assert(!bitset_contains(bs, 1000));
  bitset_del(bs);
}

// --- Multiple bits ---

TEST(test_multiple_same_chunk) {
  Bitset* bs = bitset_new();
  for (uint32_t i = 0; i < 64; i++) {
    bitset_add_bit(bs, i);
  }
  assert(bitset_size(bs) == 64);
  for (uint32_t i = 0; i < 64; i++) {
    assert(bitset_contains(bs, i));
  }
  assert(!bitset_contains(bs, 64));
  bitset_del(bs);
}

TEST(test_multiple_across_chunks) {
  Bitset* bs = bitset_new();
  bitset_add_bit(bs, 0);
  bitset_add_bit(bs, 64);
  bitset_add_bit(bs, 128);
  bitset_add_bit(bs, 200);
  assert(bitset_size(bs) == 4);
  assert(bitset_contains(bs, 0));
  assert(bitset_contains(bs, 64));
  assert(bitset_contains(bs, 128));
  assert(bitset_contains(bs, 200));
  assert(!bitset_contains(bs, 1));
  assert(!bitset_contains(bs, 65));
  bitset_del(bs);
}

// --- Large offset ---

TEST(test_large_offset) {
  Bitset* bs = bitset_new();
  bitset_add_bit(bs, 10000);
  assert(bitset_contains(bs, 10000));
  assert(!bitset_contains(bs, 9999));
  assert(!bitset_contains(bs, 10001));
  assert(bitset_size(bs) == 1);
  bitset_del(bs);
}

// --- Or ---

TEST(test_or_basic) {
  Bitset* a = bitset_new();
  Bitset* b = bitset_new();
  bitset_add_bit(a, 1);
  bitset_add_bit(a, 3);
  bitset_add_bit(b, 2);
  bitset_add_bit(b, 3);
  Bitset* c = bitset_or(a, b);
  assert(bitset_contains(c, 1));
  assert(bitset_contains(c, 2));
  assert(bitset_contains(c, 3));
  assert(!bitset_contains(c, 0));
  assert(bitset_size(c) == 3);
  bitset_del(a);
  bitset_del(b);
  bitset_del(c);
}

TEST(test_or_different_sizes) {
  Bitset* a = bitset_new();
  Bitset* b = bitset_new();
  bitset_add_bit(a, 0);
  bitset_add_bit(b, 200);
  Bitset* c = bitset_or(a, b);
  assert(bitset_contains(c, 0));
  assert(bitset_contains(c, 200));
  assert(bitset_size(c) == 2);
  bitset_del(a);
  bitset_del(b);
  bitset_del(c);
}

TEST(test_or_empty) {
  Bitset* a = bitset_new();
  Bitset* b = bitset_new();
  Bitset* c = bitset_or(a, b);
  assert(bitset_size(c) == 0);
  bitset_del(a);
  bitset_del(b);
  bitset_del(c);
}

// --- And ---

TEST(test_and_basic) {
  Bitset* a = bitset_new();
  Bitset* b = bitset_new();
  bitset_add_bit(a, 1);
  bitset_add_bit(a, 3);
  bitset_add_bit(a, 5);
  bitset_add_bit(b, 3);
  bitset_add_bit(b, 5);
  bitset_add_bit(b, 7);
  Bitset* c = bitset_and(a, b);
  assert(!bitset_contains(c, 1));
  assert(bitset_contains(c, 3));
  assert(bitset_contains(c, 5));
  assert(!bitset_contains(c, 7));
  assert(bitset_size(c) == 2);
  bitset_del(a);
  bitset_del(b);
  bitset_del(c);
}

TEST(test_and_disjoint) {
  Bitset* a = bitset_new();
  Bitset* b = bitset_new();
  bitset_add_bit(a, 0);
  bitset_add_bit(b, 1);
  Bitset* c = bitset_and(a, b);
  assert(bitset_size(c) == 0);
  bitset_del(a);
  bitset_del(b);
  bitset_del(c);
}

TEST(test_and_different_sizes) {
  Bitset* a = bitset_new();
  Bitset* b = bitset_new();
  bitset_add_bit(a, 0);
  bitset_add_bit(a, 200);
  bitset_add_bit(b, 0);
  Bitset* c = bitset_and(a, b);
  assert(bitset_contains(c, 0));
  assert(!bitset_contains(c, 200));
  assert(bitset_size(c) == 1);
  bitset_del(a);
  bitset_del(b);
  bitset_del(c);
}

TEST(test_and_empty) {
  Bitset* a = bitset_new();
  Bitset* b = bitset_new();
  Bitset* c = bitset_and(a, b);
  assert(bitset_size(c) == 0);
  bitset_del(a);
  bitset_del(b);
  bitset_del(c);
}

// --- Size ---

TEST(test_size_popcount) {
  Bitset* bs = bitset_new();
  for (uint32_t i = 0; i < 256; i += 3) {
    bitset_add_bit(bs, i);
  }
  uint32_t expected = 0;
  for (uint32_t i = 0; i < 256; i += 3) {
    expected++;
  }
  assert(bitset_size(bs) == expected);
  bitset_del(bs);
}

int main(void) {
  printf("bitset\n");

  RUN(test_new_del);
  RUN(test_add_contains);
  RUN(test_clear_bit);
  RUN(test_clear_empty);
  RUN(test_contains_empty);

  RUN(test_multiple_same_chunk);
  RUN(test_multiple_across_chunks);

  RUN(test_large_offset);

  RUN(test_or_basic);
  RUN(test_or_different_sizes);
  RUN(test_or_empty);

  RUN(test_and_basic);
  RUN(test_and_disjoint);
  RUN(test_and_different_sizes);
  RUN(test_and_empty);

  RUN(test_size_popcount);

  printf("all ok\n");
  return 0;
}
