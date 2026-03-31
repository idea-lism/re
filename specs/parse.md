### Overview

Parser generator combining visibly pushdown automata (VPA) and parsing expression grammar (PEG).

- VPA is a set of minimized DFAs with a scope stack
  - it can emit nested token stream
  - it can be extended by hook predicates
- PEG work on the token level, not character level
  - PEGs are separated / nested by the corresponding VPA scope
  - so for each child PEG the rule count is much smaller

File format:

```conf
[[vpa]]

# comment: define vpa_rules
...

[[peg]]

# comment: define peg_rules
```

The overall handling with this syntax:

1. Create `src/header_writer.c` to make C header generating a bit easier.
2. implement a manual pushdown automata in `src/parse.c`, utilizing lexers defined by `src/parse_gen.c`.
3. implement recursive descendent parsers in `src/parse.c` to:
   - recursive descend parse the whole source
   - utilize [token chunk](src/token_chunk.h)

Resulting interface:

- `bool parse_nest(const char* ustr_src, HeaderWriter* header_writer, IrWriter* ir_writer)`
  - `ustr_src` must be a ustr

For reference, file `specs/bootstrap.nest` contains the full syntax definition in its own syntax.

### The nested word `[[vpa]]`

Visibly pushdown (nested word) definition, served as lexer. Impl in `src/vpa.c`

Act as an low-level ambiguity resolver, we prefer first-declared action when possible.

The lexer will remain simple, just to solve the state and the macro problem.

Hooks have the info of current pos, parsed range, can return token id or end / fail.

to lex the syntax we mainly have these patterns defined:
- `%ignore` define tokens to ignore -- these tokens won't get into the stream
- `%effect` (for validation-only) define effect of a hook. hooks without `%effect` won't emit token / end lexing loop
- `.begin` primitive_hook: begin a scope (named by current rule name), later tokens will be pushed to a new chunk
- `.end` primitive_hook: end scope, back to parent token stream
- `.unparse` primitive_hook: put back the matched text so the next rule can re-match it
- `.fail` primitive_hook: fails parsing
- `.lit` primitive_lit: emit literal token (auto-named to "lit.xxx"). in peg the strings are auto named "lit.xxx" to match these tokens.
- `[a-z_]\w*` id
- `\*{ID}` macro rule, can be used inside a scope to inline definitions
- `\.{ID}` hook_id
- `\@{ID}` tok_id to emit
- `(b|i|ib|bi)?\/` ... `/` regexp (must not be empty) -- should handle it with child DFA
- `["'']` ... `$last_quote` quoted string literal, also generates automata
- `=` assign
- `|` or
- `{` scope_begin
- `\n[[peg]]` ends the vpa parser, go to the peg parser
- comment, space, nl

First implementation of the syntax, must be a manual recursive descendant parser.

the semantic:
- `main` is the entrance
- `%define` creates re_frag database, when parsing, expand defines
- `.lit` is syntax sugar which will be expanded at [post_process](post_process.md)
- user_hook_ids are user-defined functions referenced via `\.{ID}` pattern. They fire during lexing for semantic actions (tracking definitions, resolving names, etc.) but don't affect the token stream unless combined with `%effect`.
- Visibly pushdown automata
  - scope
    - a union of vpa_rules
    - takes up a token in parent scope
  - lookahead-1 for greedy match: when there can be no action to emit, the last min action wins (see also the MIN-RULE in aut.md)
- `\@{id}` means put tok_id into the token streaming token tree (see below)
- when a vpa scope ends, if there is a `peg_rule` with the same id as the `vpa_rule`, invoke peg parsing on the scope tokens.
  - non-peg vpa's -- don't create sub-stream (see below example)
  - non-vpa peg's -- just a peg sub-rule, no special handling

in addtion, the `re` scope has interpolation syntax:
- `\{UPPER_CASED_ID}` references a predefined regexp fragment

token stream at runtime looks like this if `scope1` is used in peg but `scope2` is not used in peg.
```
tok1 tok2 scope1 tok7 tok8
            |
         tok3 tok4 (scope2 begins) tok5 tok6 (scope2 ends)
```

### The peg `[[peg]]`

Define parsing expression grammar

tokens include:
- `[a-z_]\w*` if after a ":", assign tag id for this branch, else is PEG rule id
- `\@{ID}` token id
- `:` ... then `ID`: tag a branch
- `=` assign
- `[` branches_begin
- `]` branches_end
- `<` join_begin
- `>` join_end
- `?` maybe, greedy
- `+` plus, PEG possesive matching
- `*` star, PEG possesive matching
- literal syntax sugar
- comment, space, nl as in vpa

branch tagging syntax
- each branch has a tag (denoted by `: the_tag`)
- if no tag given, it is auto-tagged by [post_process](post_process.md).

PEG semantics:
- if there's id named `main` , it is the entrance. there must be one entrance
- besides basic peg semantics, we have "join" semantics which interlaces the "operator" inside angle brackets with the base_multi_unit

### Nested word lexing

in the bootstraping `parse.c`, define `_lex_scope(scope_id)` function, which finds corresponding scope config and lex.

```c
typedef ScopeConfigs ScopeConfig*;

struct ScopeConfig {
  int32_t scope_id;
  LexFunc lex_fn;
  ParseFunc parse_fn;
}
```

In the loop of `_lex_scope`
- when met `end_token`, return and pop chunk head.
- when a token_id is a scope_id (tok_id <= SCOPE_MAX), call `_lex_scope(tok_id)`.

Error handling:
- when sub lexer meets error, set error to lexing state
- parent lexer checks lexing state after calling sub lexer, if error, just return

The [token stream tree](src/tok_chunk.c) is organized the same way as `TokenTree` in [VPA generated code](specs/vpa.md).

The parsing also follows this nested structure:
- When parsing matches to a rule that is not a scope, call it as in normal recursive descendant parsers
- When parsing matches to a rule that maps to a scope, read a scope_id token and the chunk it points to, and call rule with the new child chunk

In summary, only `ACTION_XXX` require special handling in the lexing loop:

```c
// single
ACTION_IGNORE, // .ignore
ACTION_BEGIN, // .begin
ACTION_END, // .end
ACTION_UNPARSE, // .unparse
ACTION_FAIL, // .fail
ACTION_STR_CHECK_END, // .str_check_end

// composite: since lexer api only accepts single action_id, multiple actions must be combined
ACTION_UNPARSE_END, // .unparse .end
ACTION_SET_QUOTE_BEGIN, // .set_quote .begin
ACTION_RE_TAG_BEGIN, // @re_tag .begin
ACTION_CHARCLASS_BEGIN_BEGIN, // @charclass_begin .begin
```

So after we called a generated `lex_xxx()` and met a mismatch / eof, we check the `last_action_id`:

```c
bool _lex_scope() {
  ScopeConfig cfg = configs[scope_id];
  while (take a codepoint from input) {
    action_id = feed codepoint to current lexer
    if (action_id is mismatch) {
      if (last_action_id > 0) {
        if (last_action_id < SCOPE_COUNT) {
          _lex_scope(ctx, last_aciton_id); // call down the lexer
        } else if (last_action_id < ACTION_COUNT) {
          switch (last_action_id) {
            case ACTION_IGNORE:
              // do nothing
            case ACTION_BEGIN:
              create new token chunk at current pos
            case ACTION_END:
              goto done
            case ACTION_UNPARSE:
              revert one token
          }
        } else if (last_action_id < LIT_COUNT) {
          emit literal token in token chunk
        } else {
          emit token in token chunk
        }
      } else {
        error
        return false
      }
    } else {
      continue feeding
    }
  }
done:
  // now the chunk is finished, call the parse function
  cfg.parse_fn(src, token_chunk);
  return true;
}
```

Scope parsing example:

```c
typedef void* DStr; // darray of chars

bool _parse_str(const char* src, TokenChunk* chunk) {
  DStr buf = darray_new(sizeof(char), 0);
  char* b = (char*)buf;
  int32_t n = (int32_t)darray_size(chunk->tokens);
  for (int32_t i = 0; i < n; i++) {
    Token* t = &chunk->tokens[i];
    switch (t->tok_id) {
      case TOK_CHAR: {
        UstrCpBuf slice = ustr_slice_cp(src, t->cp_start);
        _push_buf(buf, slice.buf);
        break;
      }
      case TOK_CODEPOINT: {
        UstrIter it = {0};
        ustr_iter_init(&it, src, t->cp_start + 1);
        int cp = re_hex_to_codepoint(src + it.byte_off, t->cp_size - 1);
        char slice[4] = {0};
        ustr_encode_utf8(slice, cp);
        _push_buf(buf, slice);
        break;
      }
      case TOK_C_ESCAPE: {
        UstrIter it = {0};
        ustr_iter_init(&it, src, t->cp_start + 1);
        char* b = (char*)buf;
        darray_push(b, re_c_escape(src[it.byte_off]));
        break;
      }
      case TOK_PLAIN_ESCAPE: {
        UstrCpBuf slice = ustr_slice_cp(src, t->cp_start + 1);
        _push_buf(buf, slice.buf);
        break;
      }
    }
  }
  darray_push(b, '\0');
  // write buf to context
  return true;
}
```

### Regexp parsing and VPA parse result

Before parsing, call `_collect_re_frags()` to iterate the token tree for the `%define` rules to build the fragment table.

When met `re_ref` sub-scope, replace the scope chunk with the `%define`-ed regexp.

Note that `%define` can reference each other, must avoid infinite recursions.

### Recursive descend parsing

- Token ids are allocated scopes too
- Recursive descend parsers work on scope level
  - the input token stream buffer is a scoped chunk
  - no expand sub-scope parsing -- sub-scopes are just a token_id match
- String literals are stored with source offset + length, no extra allocations for them
- Regexps are converted to [IR](re_ir.md)
- When a scope is complete (at `.end` hook), it should invoke recursive descend parsing on the token stream chunk

### What is FORBIDDEN, a no-go

- Re-inventing regexp/lexer machine in parse.c
- Stub implementations
