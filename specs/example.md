An example show casing the capability and debuggability

- `example/drive.c`: use the lex api to create LLVM IR `lex.ll`
  - syntax: a simple language with keywords (`if`, `else`, `while`),
    identifiers (`[a-zA-Z_]\w*`), numbers (`\d+`), whitespace (`\s+`),
    and single-char operators (`=`, `+`, `(`, `)`, `{`, `}`, `;`)
  - keep `lex.ll` in example folder for later inspect
  - use `__LINE__` to help resulting DebugInfo to locate regexp strings
- `example/main.c`:
  - use `src/ustr.h`'s interface to iterate source char by char
  - use generated `lex.ll` to lex string
- `example/build.sh`: to build example
  - use `$CC` or just fallback to `clang`

