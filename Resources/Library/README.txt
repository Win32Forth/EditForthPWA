EditForthPWA library
---------------------
This tree came from EditForth. Library holds the samples: Benchmarks,
Pascal, and Sample. Debugger, Sources, and Testing live in the
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
  FROMLIB FLOAD Sample/RINGS.fth           \ then: RINGS (random COLOR8 rings; any key stops)
  FROMLIB FLOAD Sample/KOKO.fth            \ then: KOKO (COLOR8 fractal; any key stops)
  FROMLIB FLOAD Sample/MANGEN.fth          \ then: MANGEN (COLOR8 Mandelbrot; S writes mandel.raw)
  FROMLIB FLOAD Sample/MANSHOW.fth         \ then: MANSHOW (pan and zoom mandel.raw)
  FROMLIB FLOAD Sample/OBJECT.fth          \ then: OBJECT-DEMO (vehicles and automobiles)
  FROMLIB FLOAD Sample/SPREAD.fth          \ then: SPREAD (26x26 sheet; Esc quits)
  FROMLIB FLOAD Sample/SZ.fth              \ then: SZ (graphics editor; Esc opens the menu)
  FROMLIB FLOAD Sample/LEDIT.fth           \ then: LEDIT (one-line editor)
  FROMLIB FLOAD Sample/ZLIST.fth           \ then: ZLIST (list a file; skip lines that start with %)
  FROMLIB FLOAD Sample/LOOK.fth            \ then: LOOK (find a string; a blank line finishes)
  FROMLIB FLOAD Sample/GLOBAL.fth          \ then: GLOBAL (replace text in one file)
  FROMLIB FLOAD Sample/ZIM.fth             \ then: ZIM or ZIM-EXPAND
  FROMLIB FLOAD Sample/ALLSPECS.fth        \ then: ALLSPECS (collect file names)
  \ Sample/LOOK.txt                        \ notes for LOOK (not a Forth program)
  FROMLIB FLOAD Sample/BANNER.fth          \ then: BANNER-DEMO (big text; any key stops)
  FROMLIB FLOAD Sample/COMPLEX.fth         \ then: COMPLEX-DEMO (complex arithmetic)
  FROMLIB FLOAD Sample/FPMATH.fth          \ then: FPMATH (square root, log, exp, arctan, sine)
  FROMLIB FLOAD Sample/FILTER.fth          \ then: FILTER (copy a file and tidy control characters)
  FROMLIB FLOAD Sample/CARD.fth            \ then: CARD (16-line card file)
  FROMLIB FLOAD Sample/TEXTFORM.fth        \ then: TEXTFORM (dot commands, writes name.prn)
  FROMLIB FLOAD Sample/SWINDOW.fth         \ then: SWINDOW (message box; a key returns)
  FROMLIB FLOAD Sample/LINES.fth           \ then: LINES (random lines; any key stops)
  FROMLIB FLOAD Sample/LINEWALK.fth        \ then: LINEWALK (a bouncing line; any key stops)
  FROMLIB FLOAD Sample/LINESCAN.fth        \ then: LINESCAN (crawling lines; + and - change the count)
  FROMLIB FLOAD Sample/GSS.fth             \ then: GSS (growing squares; any key stops)
  FROMLIB FLOAD Sample/SPLAT.fth           \ then: SPLAT (wandering dots; Esc stops if no password)
  FROMLIB FLOAD Sample/DRAW.fth            \ then: DRAW (mouse sketch; Esc returns)
  FROMLIB FLOAD Sample/ICON.fth            \ then: ICON (40x40 color icon; Esc returns)
  FROMLIB FLOAD Sample/SSBLIT.fth          \ then: SSBLIT (logo and a moving Z; any key stops)
  \ REF / XREF / USEDIN / CALLS / ANYWORDS — cold-loaded (forth/xref.fth)
  \   REF DUP          \ colon/DEFER refs; Space=pause Esc/Q=stop; title shows leaf:line
  \   ANYWORDS OVER    \ WORDS-like across FORTH + named VOCABULARYs (optional filter)
  \   (Library/TCOM is not in this tree. Do not FLOAD Sample/XREF.)

Pascal (Tiny Pascal → Forth)
----------------------------
  Load:    FROMLIB FLOAD Pascal/PASCAL.fth
  Words:   PASCAL"  PASCAL  PASCAL-TO-FILE   (.pas only; FROMLIB via PAS-RESOLVE)
  Samples: PASY.PAS / PASY-SAMPLE.fth (clear demo), PASX.PAS / PASX-SAMPLE.fth
  Docs:    Library/Pascal/README.txt

See Testing/ANSValidate/README.txt, Pascal/README.txt, and the project README.
