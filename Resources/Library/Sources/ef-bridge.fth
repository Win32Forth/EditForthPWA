\ ef-bridge.fth — host words the cold blobs call and do not define.
\ Loaded after kernel1.fth and before kernel2.fth.
\
\ MS@, WORDLISTS, TEXTEDIT, DBG-ON, DBG-OFF, REDEF-WARNING, RESOLVE-KEY,
\ BEGIN-LOAD-CWD, END-LOAD-CWD, REGISTER-INCLUDED-STR, PARSE-FILESPEC,
\ (LOAD-ENTER), (LOAD-RUN), and (LINE-SOURCE) are already engine words.
\ This file does not replace them. FLAG_IMM is the 32-bit constant in kernel2.fth.
.( Loading: ef-bridge.fth) CR

\ ( c-addr u -- c-addr u content skip )  skip includes the line ending.
: (TZ-EOL) ( c-addr u -- c-addr u content skip )
  DUP 0= IF 0 0 EXIT THEN
  DUP 0 DO
    OVER I + C@ DUP 10 = SWAP 13 = OR IF
      I
      I 1+
      3 PICK I + C@ 13 =
      I 1+ 3 PICK < AND IF
        3 PICK I 1+ + C@ 10 = IF 1+ THEN
      THEN
      UNLOOP EXIT
    THEN
  LOOP
  DUP DUP ;
