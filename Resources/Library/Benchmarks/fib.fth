: fib ( n1 -- n2 )
    dup 2 < if
       drop 1
    else
       dup
       1- recurse
       swap 2 - recurse
       +
    then ;

: fibs ( n1 -- )
  MS@ >R     \ total time
  0
  DO    I DUP 3 .R ."  FIB  took - " MS@ >R fib MS@ R> - .ELAPSED CR PAUSE
  LOOP  ." All FIBs total - " MS@ R> - .ELAPSED CR
  ;

: main 25 fibs drop ;
.( Type main to run fibs) CR
