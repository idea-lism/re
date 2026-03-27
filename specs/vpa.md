src/vpa.c is a vpa lexer generator.

It iterates the parsed & desugared AST, utilize src/re.h to generate DFA, utilize src/irwriter.h to generate the upper level visibly pushdown machine.

### Scope handling

Every scope, except the `main`, has a starting regex. After the starting regex is matched, following hooks or tokens can be emitted, or get into child scope looping.

The built-in hook `.begin` / `.end` denotes when to push / pop a TokenChunk. When popping a chunk (scope), if it is mapped to a PEG rule, invoke the PEG parsing -- this is known in compile time, so generated code will hard-code the parsing funciton call for the `.end` action.

### From byte stream to codepoint stream to token stream

The input is byte stream, with [ustr](ustr.md), we already have a bitmap to index the starting positions of codepoints.

By feeding the chars one by one, the lexer construct another bitmap to index the newlines: each bit represents a codepoint, `1` represents the codepoint is a newline. With this index, given a cp_offset, we can quickly locate the line and column by popcnt instruction.

So the data structures are:

```c
struct TokenTree {
  uint64_t newline_map[];
  uint64_t token_end_map[];
  TokenChunk* root;
  TokenChunk* current;
}

struct TokenChunk { // matches a scope
  int32_t chunk_id;
  int32_t scope_id;
  Token tokens[];
}

// 16 bytes a token
union Token {
  int32_t tok_id; // or scope_id, parse analysis should give a universal numbering to all of them
  // with cp_start (absolute offset relative to input string):
  // - we can locate the line & column with newline_map
  // - we can locate the byte offset by ustr
  int32_t cp_start;
  int32_t cp_size;
  int32_t chunk_id; // when tok_id is a scope, it can be expanded to a TokenChunk
}

struct ChunkTable {
  TokenChunk chunks[]; // indexed by chunk_id
}
```

### Usage interface for generated code

generated header defines interface to interact with the LLVM-IR defined parser.

- user must define `%state` and `.` hooks for lexing
- each state is an opaque type `void*`, user have custom interpretation of it (string? number? string array? etc).
  - in hooks user can update states.
  - if states are used in matching, user also need to implement `match_{state_id}(const char*, userdata)` so lexer can do dynamic matching by current state. for example:
    - `foo = $foo` means the matching of the `foo` rule is totally customed by `match_foo(current_src_pointer, userdata)`
