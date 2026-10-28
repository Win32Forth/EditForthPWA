\ EditForthPWA high-level words. Cells are 4 bytes.
\ forth/kernel1.fth loads after this file and redefines the layout words.
\ IMM is bit 31 of the flag cell at CFA-4. The body stays at CFA+8.

: CELL+ ( addr -- addr' ) CELL + ;
: CELLS ( n -- n' ) CELL * ;
: CHAR+ ( addr -- addr' ) 1+ ;
: CHARS ( n -- n ) ;
: ALIGNED ( addr -- addr' ) CELL 1- + CELL 1- INVERT AND ;
: ALIGN ( -- ) HERE ALIGNED HERE - ALLOT ;
: 2DUP ( n1 n2 -- n1 n2 n1 n2 ) OVER OVER ;
: 2DROP ( n1 n2 -- ) DROP DROP ;
: 2SWAP ( n1 n2 n3 n4 -- n3 n4 n1 n2 ) ROT >R ROT R> ;
: 2OVER ( n1 n2 n3 n4 -- n1 n2 n3 n4 n1 n2 ) >R >R 2DUP R> R> 2SWAP ;
: COUNT ( c-addr -- addr u ) DUP C@ SWAP CHAR+ SWAP ;
: >= ( n1 n2 -- flag ) < 0= ;
: <= ( n1 n2 -- flag ) > 0= ;
: MIN ( n1 n2 -- n ) 2DUP > IF SWAP THEN DROP ;
: MAX ( n1 n2 -- n ) 2DUP < IF SWAP THEN DROP ;
: ?DUP ( n -- n n | 0 ) DUP IF DUP THEN ;
: SPACES ( n -- ) DUP 0> IF 0 DO SPACE LOOP ELSE DROP THEN ;
: NFA ( xt -- nfa ) DUP >FLAGS @ 65535 AND - ;
: >NAME ( xt -- nfa ) NFA ;
: HFA ( xt -- hfa ) DUP >FLAGS @ 16 RSHIFT 65535 AND - ;
: NAME>STRING ( xt -- c-addr u ) NFA DUP C@ 127 AND SWAP CHAR+ SWAP ;
: ON ( addr -- ) 1 SWAP ! ;
: OFF ( addr -- ) 0 SWAP ! ;
: DECIMAL ( -- ) 10 BASE ! ;
: HEX ( -- ) 16 BASE ! ;
\ FFA1 is the cell after >FLAGS. Line is the low half. File-id is 13 bits.
: VIEW-LINE ( xt -- u ) >FLAGS CELL+ @ 65535 AND ;
: VIEW-FILE# ( xt -- u ) >FLAGS CELL+ @ 16 RSHIFT 8191 AND ;
