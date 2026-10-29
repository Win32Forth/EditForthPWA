# Throw codes

`CATCH` and `THROW` follow the Exception word set. `ABORT` is `-1 THROW`. `ABORT"` types its message and throws −2 when the flag is nonzero. An uncaught throw prints the message, then `throw` and the code. `0 THROW` does nothing.

The published list is Forth 2012, Exception word set. This image uses these codes:

| Code | Meaning here |
|------|----------------|
| −1 | `ABORT`, and the step limit inside the inner interpreter |
| −2 | `ABORT"` |
| −3 | Data-stack underflow |
| −4 | Data-stack overflow |
| −5 | Return-stack underflow |
| −6 | Return-stack overflow |
| −8 | Dictionary full, or an allocation that cannot be satisfied |
| −9 | Division by zero (`/` `/MOD` `MOD`) |
| −13 | Undefined word |
| −14 | Interpreting a compile-only word |
| −16 | Bad address, a missing delimiter, a bad name, or a control-stack fault |
| −21 | An operation this host does not provide. With no key source, `KEY` uses this code |
| −22 | Unbalanced control flow (`UNTIL` without `BEGIN`, and the same family) |
| −38 | File not found (`can't open:`) |
| −44 | Float stack overflow, or a float that does not fit the integer result |
| −45 | Float stack underflow |

File words such as `OPEN-FILE` return an ior on the stack. A missing `INCLUDE` throws −38 instead. The Hayes exception section is part of the run that finished with `EERRORS` at 0.
