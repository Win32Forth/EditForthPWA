\ ef-hooks.fth — host words the library expects after the cold blobs.
\ Loaded after xref.fth.
\ FORTH>GRAPHICS is here. FORTH>SYSVOC stays in SYSVOC.
.( Loading: ef-hooks.fth) CR
ONLY FORTH DEFINITIONS

\ --- Debugger hook cells (vocsys would move these into SYSVOC) ---
VARIABLE DBG-SHOW-XT
VARIABLE DBG-SYNC-OK
VARIABLE DBG-HL-XT
VARIABLE DBG-WHEEL-XT
VARIABLE DBG-PAUSE-XT
VARIABLE DBG-KEY-XT
VARIABLE DBG-NEED-INTRO
VARIABLE DBG-NEED-STACKS
VARIABLE DBG-HELP-SHOWN
VARIABLE DBG-SKIP-NL
VARIABLE DBG-MIDLINE
VARIABLE DBG-LINE-COL

: DBG-CURSOR-ON ( -- ) ;
: DBG-CURSOR-OFF ( -- ) ;
: DBG-WHEEL-DO ( u -- ) DROP ;

CREATE BREAK-TABLE 8 CELLS ALLOT
BREAK-TABLE 8 CELLS ERASE
CREATE BREAK-ENABLES 8 CELLS ALLOT
BREAK-ENABLES 8 CELLS ERASE

\ Host open-at-line. The page shows that file in view mode. No host: no-op.
: (EDIT-AT) ( c-addr u line -- ) (HOST-VIEW) ;
: EDIT-AT ( c-addr u line -- ) (EDIT-AT) ;

\ Search order first, then every named vocabulary, same as a soft VIEW.
\ (SEE-HDR) and (SEE-LOC) live in SYSVOC.
ALSO SYSVOC
: (VIEW-XT) ( c-addr u -- xt true | false )
  PAD PLACE
  PAD FIND DUP IF DROP TRUE EXIT THEN
  2DROP
  XREF-COLLECT-WIDS
  XREF-WIDN @ 0 ?DO
    PAD COUNT XREF-WIDS I CELLS + @ SEARCH-WORDLIST
    IF TRUE UNLOOP EXIT THEN
  LOOP
  FALSE ;
: VIEW ( "name" -- )
  PARSE-NAME DUP 0= IF 2DROP EXIT THEN
  2DUP (VIEW-XT) 0= IF
    ."undefined: " TYPE CR EXIT
  THEN
  NIP NIP
  (SEE-HDR) (SEE-LOC)
  DUP VIEW-FILE# 0= IF DROP EXIT THEN
  DUP VIEW-LINE >R
  DUP VIEW-FILE# VIEW-PATH
  DUP 0= IF 2DROP R> DROP DROP EXIT THEN
  R> (EDIT-AT) DROP ;
PREVIOUS

\ Heads store CFAs. >LINK is the link field.
: (TZ-CFA) ( cfa -- cfa ) ;

\ Same bucket FIND uses: h = h * 33 + uppercase(c), then mod DICT-THREADS.
\ Signed MOD of a negative hash is adjusted so the bucket stays 0 .. threads-1.
: (TZ-THREAD) ( cfa -- thread )
  NAME>STRING
  0 SWAP
  0 DO
    OVER C@ >UPPER
    SWAP 33 * +
    SWAP CHAR+ SWAP
  LOOP
  NIP
  DICT-THREADS MOD
  DUP 0< IF DICT-THREADS + THEN ;

\ ( xt wid -- cfa true | false )
\ The slot is the cell that points at the current header. DO keeps its index
\ on the return stack, so the searched xt lives in a local.
: (TZ-UNLINK) ( xt wid -- cfa true | false )
  {: xt wid | slot cfa -- :}
  DICT-THREADS 0 DO
    wid I CELLS + TO slot
    BEGIN
      slot @ TO cfa
      cfa
    WHILE
      cfa xt = IF
        cfa >LINK @ slot !
        cfa UNLOOP TRUE EXIT
      THEN
      cfa >LINK TO slot
    REPEAT
  LOOP
  FALSE ;

\ ( cfa wid -- )  link cfa onto the front of its hash thread
: (TZ-LINK) ( cfa wid -- )
  OVER (TZ-THREAD) CELLS +
  DUP >R @ OVER >LINK ! R> ! ;

WARNING @
WARNING OFF

: XT>WL-FROM ( xt from-wid to-wid -- )
  >R
  (TZ-UNLINK) IF
    R> (TZ-LINK)
  ELSE
    R> DROP
    ." XT>WL: not in source wid" CR
  THEN ;

: XT>WL ( xt to-wid -- )
  FORTH-WORDLIST SWAP XT>WL-FROM ;

\ S" already dropped one blank. Skip another only if the name still starts with one.
: FORTH>WL ( c-addr u wid -- )
  >R
  DUP IF OVER C@ BL = IF SWAP CHAR+ SWAP 1- THEN THEN
  2DUP FORTH-WORDLIST SEARCH-WORDLIST
  DUP 0= IF DROP 2DROP R> DROP EXIT THEN
  DROP >R 2DROP R> R> XT>WL ;

\ VOC-WID is `2 CELLS +`. Cold load may be FORTH-only, so use the body here.
: FORTH>VOC ( c-addr u vocab-xt -- )
  2 CELLS + FORTH>WL ;

: FORTH>EDITOR ( c-addr u -- ) ['] EDITOR FORTH>VOC ;
: FORTH>GRAPHICS ( c-addr u -- ) ['] GRAPHICS FORTH>VOC ;
\ FORTH>SYSVOC stays in SYSVOC. This file does not define it.

WARNING !

\ ( a b c -- c a b )
: -ROT ( a b c -- c a b ) ROT ROT ;

: SZ-HIGHLIGHT-SPAN ( addr u -- ) 2DROP ;
: SZ-HIGHLIGHT-NAME ( c-addr u -- ) 2DROP ;

\ kernel2 [IF]/[ELSE]/[THEN] stay. A replacement needs a skip that survives
\ from one EVALUATE line to the next, and EVALUATE is not in this build.

\ kernel2's EKEY>FKEY leaves x under a miss flag. Consume the event on a miss.
WARNING @
WARNING OFF
: EKEY>FKEY ( x -- k true | false )
  DUP $FF000000 AND $02000000 = IF $FFFFFF AND TRUE ELSE DROP FALSE THEN ;
WARNING !

ONLY FORTH DEFINITIONS

\ Library debugger. Not Autoload. This file loads the other Debugger sources.
FROMLIB S" Debugger/debugger.fth" INCLUDED
.( Finished Loading: debugger.fth) CR
