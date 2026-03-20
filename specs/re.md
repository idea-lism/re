Regexp building helpers

Some automatic state management.

On top of aut.

- Creating and destructing builder: `re_new(aut)`, `re_del()`.
- Be able to negate char groups by iterating the negated ranges
  - `struct Range { start, end }`
  - `for (re_neg_ranges(iter, size_t sz, Range* ranges); !iter.end; re_neg_next(iter) ...`
    - negation stops at max unicode codepoint (0x10FFFF)
- Regexp group management
  - stack top is 0 by default
  - `re_lparen(re)` push a stack (left paren)
  - `re_fork(re)` start a new branch, forked at stack top
  - `re_range(re, start_cp, end_cp)`
  - `re_seq(re, ch1, ch2, ch3, ...)` a quick path to put in a sequence
    - chs are int32_t codepoints
  - `re_rparen(re)` pop a stack state
  - `re_action(re, action_id)` make it emit action at current state
