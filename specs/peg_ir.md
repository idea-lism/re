To help generate the PEG IR easier, create `peg_ir_xxx` helpers in src/peg_ir.c, which wraps irwriter.

The PEG IR should have instructions like:

- call: call other rules
- tok: match a token id
- push: push stack

And extend as the peg implementation needs. The goal is to lessen the burden of IR generating in src/peg.c.
