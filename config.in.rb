lib "ustr",
  srcs: %w[src/ustr.c src/ustr_arm64.c]

exe "test_ustr",
  srcs: %w[test/test_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "bench_ustr",
  srcs: %w[test/bench_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

debug   cflags: "-O0 -g -fsanitize=address -fsanitize=undefined"
release cflags: "-O2"
