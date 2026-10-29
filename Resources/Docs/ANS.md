# ANS word sets

This is not a formal ANS certificate. The checks are the two suites
in `Resources/System/Testing`.

On the page, after a reload of this engine:

- `FROMLIB FLOAD Testing/HayesTest/HayesTest.fth` prints a please-wait
  line, then runs the suite. When Core prints
  `PLEASE TYPE UP TO 80 CHARACTERS:`, the page queues `hayes-accept`
  and Return, so `ACCEPT` does not wait. The run finished with every
  section counter at 0, including `FPERRORS` and `#ERRORS`, and printed
  `HAYES: ALL COUNTS ZERO — PASS`.
- `FROMLIB FLOAD Testing/ANSValidate/ANS-VALIDATE.fth` prints a
  please-wait line, then loads tester, core, core-ext, search, string,
  facility, exception, memory, double, locals, tools, file, block,
  xchar, float, and host. It finished with no error.

Cells are 4 bytes. `ENVIRONMENT?` `FLOATING` and `FLOAT-EXT` are true.
`FLOATING-STACK` is 16. Float words live in the `FLOATING` vocabulary
and use a separate stack of IEEE doubles. `/` `/MOD` `MOD` throw −9 on
a zero divisor and round toward zero. `FLOORED` is false.

Canvas graphics are not wired up. A sample that draws still loads, and
it does not open a window.
