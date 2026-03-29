OPTION(o, output, 1, "output LLVM IR file")
OPTION(f, func, 1, "function name (default: \"lex\")")
OPTION(m, mode, 1, "mode flags: i=case-insensitive, b=binary")
OPTION(t, triple, 2, "target triple (default: probe clang)")
#undef OPTION
