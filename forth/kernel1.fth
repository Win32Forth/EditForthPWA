\ EditForthPWA 32-bit copy of the EditForth cold blob.
\ Cells are 4 bytes. The body is at CFA+8. IMM is bit 31 of the flag cell
\ at CFA-4. The EditForth original is unchanged.
\ Dictionary field helpers (xt = CFA from ' or FIND):
\   >LINK  ( xt -- a-addr )  link at CFA-12
\   >FLAGS ( xt -- a-addr )  FFA0 at CFA-8 (NFA offset, HFA offset)
\   >CODE  ( xt -- a-addr )  CFA itself
\   >BODY  ( xt -- a-addr )  CFA+8
.( Loading: kernel1.fth) CR
\ DOC" needs SETDOC. Define DOC" first, then document HERE via redefine.
: DOC" 34 PARSE SETDOC ;
DOC" HERE ( -- addr ) current dictionary pointer (DP @)"
: HERE DP @ ;

\ --- 1. Simple ANS helpers ---
DOC" CHAR+ ( addr -- addr' ) add size of one char"
: CHAR+ 1+ ;
DOC" CHARS ( n -- n ) chars to address units"
: CHARS ;
DOC" CELL+ ( addr -- addr' ) add size of one cell"
: CELL+ CELL + ;
DOC" CELLS ( n -- n ) cells to address units"
: CELLS CELL * ;
DOC" ALIGNED ( addr -- addr' ) next aligned address"
: ALIGNED CELL 1- + CELL 1- INVERT AND ;
DOC" ALIGN ( -- ) align DP to cell boundary"
: ALIGN HERE ALIGNED HERE - ALLOT ;
DOC" 2DUP ( n1 n2 -- n1 n2 n1 n2 ) duplicate pair"
: 2DUP OVER OVER ;
DOC" 2DROP ( n1 n2 -- ) drop two items"
: 2DROP DROP DROP ;
DOC" 2SWAP ( n1 n2 n3 n4 -- n3 n4 n1 n2 ) swap pairs"
: 2SWAP ROT >R ROT R> ;
DOC" 2OVER ( n1 n2 n3 n4 -- n1 n2 n3 n4 n1 n2 ) copy second pair"
: 2OVER >R >R 2DUP R> R> 2SWAP ;
DOC" COUNT ( c-addr -- addr u ) from counted string addr return char-addr and length"
: COUNT DUP C@ SWAP CHAR+ SWAP ;
DOC" /STRING ( c-addr u n -- c-addr' u' ) adjust string by n characters"
: /STRING DUP >R - SWAP R> + SWAP ;
DOC" DECIMAL ( -- ) set BASE to 10"
: DECIMAL 10 BASE ! ;
DOC" HEX ( -- ) set BASE to 16"
: HEX 16 BASE ! ;
\ 0<> 0> WITHIN U> are engine primitives
DOC" >= ( n1 n2 -- flag ) greater or equal"
: >= < 0= ;
DOC" <= ( n1 n2 -- flag ) less or equal"
: <= > 0= ;

\ --- 2. Dictionary field accessors (xt = CFA) ---
DOC" >LINK ( xt -- a-addr ) link field address"
: >LINK 3 CELLS - ;
DOC" >FLAGS ( xt -- a-addr ) FFA0 address (NFA and HFA offsets)"
: >FLAGS 2 CELLS - ;
DOC" >CODE ( xt -- a-addr ) code field (xt itself)"
: >CODE ;
DOC" >BODY ( xt -- addr ) body at CFA+8 (colon code, or CREATE/DOES> data)"
: >BODY 2 CELLS + ;
\ Layout: HFA | NFA | LFA | FFA0 | FFA1 | CFA | INFO | BODY
\ NFA count byte: bits 0-6 = length (max 127), bit 7 = SMUDGE (hidden until ;)
\ FFA0: 0-15 NFA offset, 16-31 HFA offset
\ FFA1 at >FLAGS CELL+: 0-15 line, 16-28 file-id, 29 INLINE, 30 EMM, 31 IMM
DOC" NFA ( xt -- nfa ) name field address"
: NFA DUP >FLAGS @ 65535 AND - ;
DOC" >NAME ( xt -- nfa ) name field address (classic synonym for NFA)"
: >NAME NFA ;
DOC" >NFA ( xt -- nfa ) synonym for >NAME / NFA"
: >NFA NFA ;
DOC" HFA ( xt -- hfa ) help field address"
: HFA DUP >FLAGS @ 16 RSHIFT 65535 AND - ;
DOC" VIEW-LINE ( xt -- u ) 1-based source line in FFA1 (0=none)"
: VIEW-LINE >FLAGS CELL+ @ 65535 AND ;
DOC" VIEW-FILE# ( xt -- u ) source file-id in FFA1 (0=none)"
: VIEW-FILE# >FLAGS CELL+ @ 16 RSHIFT 8191 AND ;
\ The traversal token in these blobs is the CFA. Callers with an NFA use it as an xt only after NFA.
DOC" NAME>STRING ( xt -- c-addr u ) name chars and length (masks NFA SMUDGE bit)"
: NAME>STRING NFA DUP C@ 127 AND SWAP CHAR+ SWAP ;
DOC" SMUDGE ( -- ) hide LAST (set NFA count bit7); also done by :"
: SMUDGE LAST NFA DUP C@ 128 OR SWAP C! ;
DOC" REVEAL ( -- ) unhide LAST (clear NFA count bit7); also done by ;"
: REVEAL LAST NFA DUP C@ 127 AND SWAP C! ;
DOC" >HELP ( xt -- hfa ) help string"
: >HELP HFA ;
-1 CONSTANT DOCOL-ADDR
-2 CONSTANT DOVAR-ADDR
-3 CONSTANT DOCON-ADDR
-4 CONSTANT DODOES-ADDR
DOC" DOCOL? ( xt -- flag ) true if colon definition"
: DOCOL? @ DOCOL-ADDR = ;
DOC" SPACE ( -- ) emit one space"
: SPACE BL EMIT ;

\ --- 3. Control flow ---
\ BEGIN UNTIL AGAIN IF ELSE THEN WHILE REPEAT AHEAD
\ DO ?DO LOOP +LOOP LEAVE
\ stay engine words. A branch offset is the destination minus the cell
\ after the offset, and that cell is 4 bytes. LEAVE shares the engine
\ control stack with DO, so these compilers are not redefined here.
DOC" ?COMP ( -- ) error if not compiling"
: ?COMP STATE @ 0= IF S" compile only" TYPE CR -14 THROW THEN ;

\ --- 4. Defining words / parse helpers ---
DOC" CHAR ( 'name' -- char ) first character of next word"
: CHAR BL WORD COUNT DROP C@ ;
DOC" [CHAR] ( 'name' -- ) compile first char of name as literal (immediate)"
: [CHAR] ?COMP CHAR ['] LIT , , ; IMMEDIATE
DOC" VARIABLE ( 'name' -- ) create a variable"
: VARIABLE CREATE 0 , ;
DOC" CONSTANT ( x 'name' -- ) create a constant"
: CONSTANT CREATE , DOES> @ ;
DOC" RECURSE ( -- ) recurse into current definition (immediate)"
: RECURSE ?COMP LAST , ; IMMEDIATE

\ --- Search-Order / VOCABULARY / NAMESPACE ---
\ CONTEXT! matches classic VOCABULARY: write search-order wid1 without growing n.
DOC" CONTEXT! ( wid -- ) replace first search-order entry (CONTEXT)"
: CONTEXT!  ( wid -- )
  >R GET-ORDER
  DUP 0= IF DROP R> 1 SET-ORDER EXIT THEN
  NIP R> SWAP SET-ORDER ;
\ (REGISTER-WID) is an engine word. It records the wid for FORGET and WORDLISTS.
DOC" VOCABULARY ( 'name' -- ) named word list; execute replaces CONTEXT (classic)"
: VOCABULARY CREATE DICT-THREADS 0 DO 0 , LOOP
  LAST >BODY (REGISTER-WID) DOES> CONTEXT! ;
\ ciforth-style push: grow search order then set CONTEXT (like ALSO + replace).
DOC" PUSH-ORDER ( wid -- ) search this wordlist first, keeping the rest"
: PUSH-ORDER ( wid -- )
  >R GET-ORDER R> SWAP 1+ SET-ORDER ;
DOC" NAMESPACE ( 'name' -- ) named word list; execute pushes onto search order"
: NAMESPACE CREATE DICT-THREADS 0 DO 0 , LOOP
  LAST >BODY (REGISTER-WID) DOES> PUSH-ORDER ;
DOC" BIG-INTEGER ( -- ) vocabulary for big-integer extensions; ALSO BIG-INTEGER to use"
VOCABULARY BIG-INTEGER
DOC" EDITOR ( -- ) vocabulary for editor extensions; ALSO EDITOR to use"
VOCABULARY EDITOR
DOC" ASSEMBLER ( -- ) vocabulary for assembler extensions; ALSO ASSEMBLER to use"
VOCABULARY ASSEMBLER
DOC" FLOATING ( -- ) vocabulary for floating-point word set; ALSO FLOATING to use"
VOCABULARY FLOATING
DOC" ONLY ( -- ) search order is just FORTH-WORDLIST"
: ONLY FORTH-WORDLIST 1 SET-ORDER ;

ONLY FORTH DEFINITIONS
