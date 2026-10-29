EditForthPWA library
---------------------
This tree came from EditForth. Library holds the samples: Benchmarks,
Classes, Pascal, and Sample. Debugger, Sources, and Testing live in the
sibling Resources/System folder, as they do in EditForth.

FROMLIB resolves a relative path under Resources/Library. The same word
finds Assembler, Debugger, Emitter, Hyper, Reference, Sources, and
Testing under Resources/System. A path written as System/Debugger/… is
Resources/System/Debugger/….

Library/TCOM, Library/BigInteger, Library/PI, System/Reference,
System/Emitter, System/Assembler, and System/Hyper are not included.
Resources/AutoLoad and Resources/Config are not included.
The ARM64 kernel (forth.s and the boot-word includes) is not included.
Boot embeds the Forth files in the project's forth/ directory. This
folder is not loaded at boot. The project README describes what currently runs.

Examples
--------
  FROMLIB FLOAD Testing/HayesTest/HayesTest.fth
  FROMLIB FLOAD Testing/ANSValidate/ANS-VALIDATE.fth
  FROMLIB FLOAD Testing/DbgSpanSmoke/span-tests.fth
  FROMLIB FLOAD xchar-smoke.fth
  FROMLIB FLOAD Debugger/debugger.fth  \ also loaded at boot from ef-hooks.fth
  FROMLIB FLOAD Pascal/PASCAL.fth          \ Tiny Pascal → Forth translator
  FROMLIB S" Pascal/PASY.PAS" PASCAL-TO-FILE   \ then INCLUDED + DEMO
  FROMLIB FLOAD Sample/DOODLE64.fth        \ then: DOODLE (1-bit mouse demo)
  FROMLIB FLOAD Sample/DOODLECOLOR64.fth   \ then: DOODLECOLOR (COLOR8 + color bar)
  FROMLIB FLOAD Sample/IMAGEVIEW64.fth     \ then: IMAGEVIEW (TRUECOLOR open/zoom)
  FROMLIB FLOAD Sample/EDIT64.fth          \ then: EDIT64 (GRAPHICS mini-editor; EMITable)
  \ REF / XREF / USEDIN / CALLS / ANYWORDS — cold-loaded (Kernel/xref.fth .incbin)
  \   REF DUP          \ colon/DEFER refs; Space=pause Esc/Q=stop; title shows leaf:line
  \   ANYWORDS OVER    \ WORDS-like across FORTH + named VOCABULARYs (optional filter)
  \   (Classic TCOM source kept at TCOM/REF.FTH; do not FLOAD Sample/XREF.)

Pascal (Tiny Pascal → Forth)
----------------------------
  Load:    FROMLIB FLOAD Pascal/PASCAL.fth
  Words:   PASCAL"  PASCAL  PASCAL-TO-FILE   (.pas only; FROMLIB via PAS-RESOLVE)
  Samples: PASY.PAS / PASY-SAMPLE.fth (clear demo), PASX.PAS / PASX-SAMPLE.fth
  Docs:    Library/Pascal/README.txt

See Testing/ANSValidate/README.txt, Pascal/README.txt, and the project README.
