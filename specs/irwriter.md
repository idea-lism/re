Simple LLVM IR writer

- For convenient text IR building -- so building does not depend on LLVM project
- simple, just write to `FILE*`
- maps state name to basic block label
- auto numbering

We target DFA generation, so there are no loops and no need for a complex Dominance-Frontier algorithm.

API

- basic: `irwriter_new(FILE*, char* target_triple)`, `irwriter_del()`
- module prelude and epilogue `irwriter_start()`, `irwriter_end()`
- function prelude and epilogue `irwriter_define_start()`, `irwriter_define_end()`
  - also with file_path, compiling cwd
- starting a basic block
- binop, binop_imm, etc
- emits debug information
- can create debugtrap

Security

- check file_name should not contain `"` or `\\`
- check function_name should not contain `"` or `\\`
- check target_triple format
