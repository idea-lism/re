To help generate the PEG IR easier, create `peg_ir_xxx` helpers in src/peg_ir.c, which wraps irwriter.

The PEG IR should have instructions like:

- call: call other rules
- tok: match a token id
- push: push stack

And extend as the peg implementation needs. The goal is to lessen the burden of IR generating in src/peg.c.

Reference IR

```
τ(ε, L) = nop
τ(c, L) = tok c
          iffail(L)
τ(A, L) = call A
          iffail(L)
τ(e e0, L) = τ(e, L)
             τ(e0, L)
τ(e / e0, L) = push
               τ(e, L')
               pop
               jump L"
            L’ peek
               pop
               succ
               τ(e0, L)
            L” nop
```
