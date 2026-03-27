The PEG IR helpers in src/peg_ir.c wrap irwriter for multi-instruction sequences. Single LLVM instructions (add, icmp, select, phi, br) are emitted directly via irwriter.

## Opcodes

All values are `i32`. Convention: negative return = failure.

| Opcode | LLVM IR expansion | Semantics |
|---|---|---|
| `tok(token_id, col)` | `call i32 @match_tok(i32 %token_id, i32 %col)` | Match token at `col`. Returns match length (≥0) or negative on failure. |
| `call(rule_id, col)` | `sext` + `call i32 @parse_{id}(ptr %table, i64 %col)` | Call rule function (which checks memo table internally). Returns match length or negative. |
| `memo_get(rule_id, col)` | `getelementptr` + `load i32` from `table->col[%col].slots[%rule_id]` | Read memo slot. Returns cached value or `-1` (uncached). |
| `memo_set(rule_id, col, val)` | `getelementptr` + `store i32 %val` into `table->col[%col].slots[%rule_id]` | Write memo slot. |

### Backtrack stack (ordered choice only)

| Opcode | Semantics |
|---|---|
| `save(col)` | Push column position onto backtrack stack. |
| `restore()` | Read saved column without popping. Returns the saved `col`. |
| `discard()` | Drop top of backtrack stack. |

The backtrack stack type and operations are emitted as `define internal` functions in the generated LLVM IR, so LLVM can inline them:

```llvm
%BtStack = type { [16 x i32], i32 }  ; { data[16], top }

define internal void @backtrack_push(ptr %stack, i32 %col) { ... }
define internal i32  @backtrack_restore(ptr %stack)         { ... }
define internal void @backtrack_pop(ptr %stack)             { ... }
```

Each rule function allocates `%BtStack` on the LLVM stack and initializes `top = -1`. The `save`/`restore`/`discard` opcodes map to `call @backtrack_push`/`@backtrack_restore`/`@backtrack_pop`.

### Memo helpers (row_shared mode)

These are used only in `row_shared` mode. In `naive` mode, `memo_get`/`memo_set` suffice.

| Opcode | LLVM IR expansion | Semantics |
|---|---|---|
| `bit_test(seg_idx, rule_bit, col)` | `getelementptr` + `load` + `and i32 %bits, %rule_bit` + `icmp ne i32 ..., 0` | Test rule's bit in the segment. Returns `i1` (1 = may match, 0 = proven fail). |
| `bit_deny(seg_idx, rule_bit, col)` | `getelementptr` + `load` + `and i32 %bits, ~%rule_bit` + `store` | Clear rule's bit (cache failure). |
| `bit_exclude(seg_idx, rule_bit, col)` | `getelementptr` + `load` + `and i32 %bits, %rule_bit` + `store` | Keep only this rule's bit, zero out others in segment (cache exclusive match). |

## Reference IR

`gen(pattern, col, on_fail)` generates IR for matching `pattern` at column `col`, branching to `on_fail` on failure. Returns match length on success.

### Primitives Translation

```
gen(empty, col, fail):
  return 0

gen(token, col, fail):
  r = tok(token, col)
  fail_if_neg(r, fail)
  return r

gen(Rule, col, fail):
  r = call(Rule, col)
  fail_if_neg(r, fail)
  return r
```

### Sequence

```
gen(a b, col, fail):
  r1 = gen(a, col, fail)
  r2 = gen(b, col + r1, fail)
  return r1 + r2
```

### Ordered choice

Refer to the branches syntax in [parse.md](parse.md).

```
gen(a / b, col, fail):
  save(col)
  r = gen(a, col, alt_bb)
  discard()
  br(done_bb)
alt_bb:
  restore()
  discard()
  r2 = gen(b, col, fail)
  br(done_bb)
done_bb:
  result = phi(r from succ_bb, r2 from alt_bb)
  return result
```

For N-ary ordered choice `a / b / c / ...`, the pattern generalizes: one `save` at the start, `restore` + `discard` + `save` at each non-last retry, `restore` + `discard` before the last attempt.

### Optional (?)

Always succeeds, never branches to fail.

```
gen(e?, col, fail):
  r = gen(e, col, miss_bb)
  br(done_bb)
miss_bb:
  br(done_bb)
done_bb:
  result = phi(r from try_bb, 0 from miss_bb)
  return result
```

### One-or-more (+), possessive

First match must succeed. Then loop greedily, no backtracking.

```
gen(e+, col, fail):
  first = gen(e, col, fail)
  br(loop_bb)
loop_bb:
  acc = phi(first from entry_bb, next from body_bb)
  r = gen(e, col + acc, end_bb)
  next = acc + r
  br(loop_bb)
end_bb:
  return acc
```

### Zero-or-more (*), possessive

Same loop as `+`, but starts with zero matches (always succeeds).

```
gen(e*, col, fail):
  br(loop_bb)
loop_bb:
  acc = phi(0 from entry_bb, next from body_bb)
  r = gen(e, col + acc, end_bb)
  next = acc + r
  br(loop_bb)
end_bb:
  return acc
```

### One-or-more with interlace (+\<sep\>)

Matches `e (sep e)*`. First element required, then alternating separator and element.

```
gen(e+<sep>, col, fail):
  first = gen(e, col, fail)
  br(loop_bb)
loop_bb:
  acc = phi(first from entry_bb, next from body_bb)
  sr = gen(sep, col + acc, end_bb)
  er = gen(e, col + acc + sr, end_bb)
  next = acc + sr + er
  br(loop_bb)
end_bb:
  return acc
```

### Zero-or-more with interlace (*\<sep\>)

Matches `(e (sep e)*)?`. Zero matches is OK.

```
gen(e*<sep>, col, fail):
  first = gen(e, col, empty_bb)
  br(loop_bb)
loop_bb:
  acc = phi(first from entry_bb, next from body_bb)
  sr = gen(sep, col + acc, end_bb)
  er = gen(e, col + acc + sr, end_bb)
  next = acc + sr + er
  br(loop_bb)
empty_bb:
  br(end_bb)
end_bb:
  result = phi(acc from loop_bb, 0 from empty_bb)
  return result
```

## Notes

- `gen` always has an `on_fail` label. On failure, control transfers there. On success, it falls through and returns the match length.
- `?` and `*` always succeed — they never branch to `on_fail`.
- `+` and `*` are possessive (no backtracking).
- `+<sep>` / `*<sep>` interlace: the separator is only consumed when followed by a successful element match. The loop exits before accumulating the separator, discarding both `sr` and `er`.
- In `row_shared` mode, rule functions use `bit_test`/`bit_deny`/`bit_exclude` before `memo_get`/`memo_set`. In `naive` mode, they use `memo_get`/`memo_set` directly.
