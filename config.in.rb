lib "ustr",
  srcs: %w[src/ustr.c src/ustr_neon.c src/ustr_avx.c]

exe "test_ustr",
  srcs: %w[test/test_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_irwriter",
  srcs: %w[test/test_irwriter.c src/irwriter.c]

exe "test_bitset",
  srcs: %w[test/test_bitset.c src/bitset.c]

exe "bench_ustr",
  srcs: %w[test/bench_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_aut",
  srcs: %w[test/test_aut.c src/aut.c src/irwriter.c src/bitset.c]

exe "test_re",
  srcs: %w[test/test_re.c src/re.c src/aut.c src/irwriter.c src/bitset.c]

exe "test_lex",
  srcs: %w[test/test_lex.c src/lex.c src/re.c src/aut.c src/irwriter.c src/bitset.c],
  deps: %w[ustr]

combined_lib "re",
  srcs: %w[src/ustr.c src/ustr_neon.c src/ustr_avx.c src/irwriter.c src/bitset.c src/aut.c src/re.c src/lex.c]

amalgamate input: "src/re_rt.h.in",
  output: "out/re_rt.h",
  include_dirs: %w[src]

dist_header "src/lex.h", to: "out/lex.h"

debug   cflags: "-O0 -g -fsanitize=address -fsanitize=undefined"
release cflags: "-O2"
