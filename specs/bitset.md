A bitset tool for DFA building.

The internal is uint64_t chunks. basic functions are:

- `bitset_new()`, `bitset_del()`
- `bitset_add_bit(offset)`, `bitset_clear_bit(offset)`
- `bitset_contains(offset)`
- `bitset_or(s1, s2)`
- `bitset_and(s1, s2)`
- `bitset_size()` - use popcnt to compute

And maybe other functions if automata requires.
