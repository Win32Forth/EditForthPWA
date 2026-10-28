EditForthPWA library
---------------------
This tree came from EditForth. Library/TCOM, Library/Reference,
Library/BigInteger, Library/PI, and Library/Emitter are not included.
The ARM64 kernel (forth.s and the boot-word includes) is not included.
Boot embeds the Forth files in the project's forth/ directory. This
folder is not loaded at boot. The project README describes what currently runs.

64Forth Library — sources under 64Forth/Resources/Library/

Build (TZForth-style Run Script "Copy Library") wipes and re-copies this
folder into the app bundle as Contents/Resources/Library/ on every build
so FROMLIB never sees a stale tree. Same for AutoLoad and Docs.

FROMLIB resolves relative paths under the bundle Library - no machine-specific
absolute paths.

Examples
--------
  FROMLIB FLOAD Testing/HayesTest/HayesTest.fth
  FROMLIB FLOAD Testing/ANSValidate/ANS-VALIDATE.fth
  FROMLIB FLOAD Testing/DbgSpanSmoke/span-tests.fth
  FROMLIB FLOAD xchar-smoke.fth
  FROMLIB FLOAD Debugger/debugger.fth  \ DEBUG hub + loads Debugger/* (AutoLoad)
  FROMLIB FLOAD Assembler/asmarm64.fth     \ AArch64 host toolkit (not used by TCOM)
  FROMLIB FLOAD Assembler/ASSEMBLER.fth    \ interactive CODE / END-CODE (64Forth only)
  FROMLIB FLOAD Assembler/ASMARMTESTS.fth \ then: ASM-TESTS
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

Assembler (ASMARM64)
--------------------
  Full source: Library/Assembler/asmarm64.fth
  Interactive: Library/Assembler/ASSEMBLER.fth  →  CODE / END-CODE / C; / NEXT,
               (64Forth ITC only; not loaded by 64TCOM; does not redefine ASSEMBLER)
  Tests:       Library/Assembler/ASMARMTESTS.fth  →  ASM-TESTS
  Twin copies: Documents/64TCOM/64TCOMARM64/ASMARM64.fth + ASMARMTESTS.fth
  Last synced: Aug 23, 2026 3:16 PM (see header stamp in both .fth files)
  TCOM loads only the pack assembler via TARGETARM64.
  Monitor: Docs/STATUSASM64.md (twin of 64TCOM/STATUSASM64.md)
  After load: .ASMARM64  ASM-TESTS  ASMARM64-DISCARD
  CODE words:  FROMLIB FLOAD Assembler/ASSEMBLER.fth  then  CODE name … END-CODE

Pascal (Tiny Pascal → Forth)
----------------------------
  Load:    FROMLIB FLOAD Pascal/PASCAL.fth
  Words:   PASCAL"  PASCAL  PASCAL-TO-FILE   (.pas only; FROMLIB via PAS-RESOLVE)
  Samples: PASY.PAS / PASY-SAMPLE.fth (clear demo), PASX.PAS / PASX-SAMPLE.fth
  Docs:    Library/Pascal/README.txt

User data (Documents/64Forth)
-----------------------------
  First launch copies Library / AutoLoad / Docs into Documents/64Forth.
  FROMLIB prefers that tree. Tools menu: Update User Data / Restore Shipped Files.

After editing Library .fth files, rebuild/run so the bundle copy updates
(or use Tools → Update / Restore for the Documents/64Forth tree).

See Testing/ANSValidate/README.txt, Editor/SZ-EDITOR-README.txt,
Pascal/README.txt, and Docs/README.txt.
