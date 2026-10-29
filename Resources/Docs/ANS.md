# ANS word sets

This is not a formal ANS certificate. The checks are the two suites in `Resources/Library/Testing`.

On the page, after a reload of this engine:

- `FROMLIB FLOAD Testing/HayesTest/HayesTest.fth` finished with every section counter at 0, including `FPERRORS` and `#ERRORS`, and printed `HAYES: ALL COUNTS ZERO — PASS`. Core stops once at `ACCEPT` and waits for a typed line.
- `FROMLIB FLOAD Testing/ANSValidate/ANS-VALIDATE.fth` finished with no error. That driver loads tester, core, core-ext, search, string, facility, exception, memory, double, locals, tools, file, block, xchar, float, and host.

Cells are 4 bytes. `ENVIRONMENT?` `FLOATING` and `FLOAT-EXT` are true. `FLOATING-STACK` is 16. Float words live in the `FLOATING` vocabulary and use a separate stack of IEEE doubles. `/` `/MOD` `MOD` throw −9 on a zero divisor and round toward zero. `FLOORED` is false.

Canvas graphics are not wired up. A sample that draws still loads, and it does not open a window.
