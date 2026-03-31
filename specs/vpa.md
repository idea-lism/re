# Visibly pushdown generator

Create `src/vpa.c` (`vpa_gen()`):
- define functions to generates visibly pushdown automata in LLVM IR, using Parser's processed-data
- generate helpers for result C header
  1. token id definitions
  2. util functions that the final LLVM IR may need

It iterates the parsed & desugared AST, utilize src/re.h to generate DFA, utilize src/irwriter.h to generate the upper level visibly pushdown machine.

### Scope handling

Every scope, except the `main`, has a starting regex. After the starting regex is matched, following hooks or tokens can be emitted, or get into child scope looping.

The built-in hook `.begin` / `.end` denotes when to push / pop a TokenChunk. When popping a chunk (scope), if it is mapped to a PEG rule, invoke the PEG parsing -- this is known in compile time, so generated code will hard-code the parsing funciton call for the `.end` action.

Sub-scope calls inlines the leader regexp as matcher. Assume we have a scope like this:

```c
# braced
s = /regex1/ {
  /regex2/
  a
}

a = /regex3/ .hook1 {
  /regex4/
}
```

The matcher for `s` first matches `/regex1/`, then the braced union. Since `a` is called, the brace in `s` should be compiled to the union of `/regex2/` and `/regex3/` (the leader regexp of `a`). And if `/regex3/` is matched, go into `a`'s following processing: 1. invoke `.hoo1`, 2. loop into `a`'s braced union with `/regex4/`.

Note that the automata's `action_id` is not token_id or scope id. each action_id should map to a sequence of actions like invoke hook / emit token, so we will have a label array, and utilize the computed-goto extension (there should be equivalent one in LLVM IR) to invoke the series of actions quickly.

### From byte stream to codepoint stream to token stream

The input is byte stream, with [ustr](ustr.md), we already have a bitmap to index the starting positions of codepoints.

By feeding the chars one by one, the lexer construct another bitmap to index the newlines: each bit represents a codepoint, `1` represents the codepoint is a newline. With this index, given a cp_offset, we can quickly locate the line and column by popcnt instruction.

So the data structures are:

```c
struct TokenTree {
  const char* src; // = ustr
  uint64_t newline_map[]; // bitmap = (ustr_size(ustr) + 63) / 64
  TokenChunk* root; // pointer to the root chunk
  TokenChunk* current; // pointer to the current chunk
  TokenChunk table[]; // indexed by chunk_id
}

struct TokenChunk { // matches a scope
  int32_t scope_id;
  int32_t parent_id; // -1 for root chunk
  Token tokens[];
}

// 16 bytes a token
struct Token {
  int32_t tok_id; // or scope_id (is scope id when < SCOPE_COUNT), parse analysis should give a universal numbering to all of them
  // with cp_start (absolute offset relative to input string):
  // - we can locate the line & column with newline_map
  // - we can locate the byte offset by ustr
  int32_t cp_start;
  int32_t cp_size;
  int32_t chunk_id; // when tok_id is a scope, it can be expanded to a TokenChunk
}
```

And helper functions:

```c
TokenTree* tc_tree_new(ustr);
void tc_tree_del(TokenTree*);
struct {line, col} tc_locate(TokenTree* tree, int32_t cp_offset);
tc_add(TokenChunk* c, Token t);
TokenChunk* tc_push(TokenTree* tree); // updates current
TokenChunk* tc_pop(TokenTree* tree);  // returns new current
```

### Usage interface for generated code

generated header defines interface to interact with the LLVM-IR defined parser.

- user must define `.` hooks for lexing
