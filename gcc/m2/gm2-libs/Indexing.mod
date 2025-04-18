(* Indexing.mod provides a dynamic indexing mechanism for CARDINAL.

Copyright (C) 2003-2025 Free Software Foundation, Inc.
Contributed by Gaius Mulley <gaius.mulley@southwales.ac.uk>.

This file is part of GNU Modula-2.

GNU Modula-2 is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GNU Modula-2 is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  *)

IMPLEMENTATION MODULE Indexing ;

FROM libc IMPORT memset, memmove ;
FROM Storage IMPORT ALLOCATE, REALLOCATE, DEALLOCATE ;
FROM SYSTEM IMPORT TSIZE, WORD, BYTE ;

CONST
   MinSize           = 128 ;
   DefaultGrowFactor = 2 ;

TYPE
   PtrToAddress = POINTER TO ADDRESS ;
   PtrToByte    = POINTER TO BYTE ;

   Index = POINTER TO RECORD
                         ArrayStart: ADDRESS ;
                         ArraySize : CARDINAL ;
                         Used,
                         Low,
                         High      : CARDINAL ;
                         Debug     : BOOLEAN ;
                         Map       : BITSET ;
                         GrowFactor: CARDINAL ;
                      END ;

(*
   InitIndexTuned - creates a dynamic array with low indice.
                    The minsize is the initial number of elements the
                    array is allocated and growfactor determines how
                    it will be resized once it becomes full.
*)

PROCEDURE InitIndexTuned (low, minsize, growfactor: CARDINAL) : Index ;
VAR
   i: Index ;
BEGIN
   NEW (i) ;
   WITH i^ DO
      Low := low ;
      High := 0 ;
      ArraySize := minsize * TSIZE (ADDRESS) ;
      ALLOCATE (ArrayStart, ArraySize) ;
      ArrayStart := memset (ArrayStart, 0, ArraySize) ;
      Debug := FALSE ;
      Used := 0 ;
      Map := BITSET {} ;
      GrowFactor := growfactor
   END ;
   RETURN i
END InitIndexTuned ;


(*
   InitIndex - creates and returns an Index.
*)

PROCEDURE InitIndex (low: CARDINAL) : Index ;
BEGIN
   RETURN InitIndexTuned (low, MinSize, DefaultGrowFactor)
END InitIndex ;


(*
   KillIndex - returns Index to free storage.
*)

PROCEDURE KillIndex (i: Index) : Index ;
BEGIN
   WITH i^ DO
      DEALLOCATE(ArrayStart, ArraySize)
   END ;
   DISPOSE(i) ;
   RETURN( NIL )
END KillIndex ;


(*
   DebugIndex - turns on debugging within an index.
*)

PROCEDURE DebugIndex (i: Index) : Index ;
BEGIN
   i^.Debug := TRUE ;
   RETURN( i )
END DebugIndex ;


(*
   InBounds - returns TRUE if indice, n, is within the bounds
              of the dynamic array.
*)

PROCEDURE InBounds (i: Index; n: CARDINAL) : BOOLEAN ;
BEGIN
   IF i=NIL
   THEN
      HALT
   ELSE
      WITH i^ DO
         RETURN( (n>=Low) AND (n<=High) )
      END
   END
END InBounds ;


(*
   HighIndice - returns the last legally accessible indice of this array.
*)

PROCEDURE HighIndice (i: Index) : CARDINAL ;
BEGIN
   IF i=NIL
   THEN
      HALT
   ELSE
      RETURN( i^.High )
   END
END HighIndice ;


(*
   LowIndice - returns the first legally accessible indice of this array.
*)

PROCEDURE LowIndice (i: Index) : CARDINAL ;
BEGIN
   IF i=NIL
   THEN
      HALT
   ELSE
      RETURN( i^.Low )
   END
END LowIndice ;


(*
   PutIndice - places, a, into the dynamic array at position i[n]
*)

PROCEDURE PutIndice (i: Index; n: CARDINAL; a: ADDRESS) ;
VAR
   oldSize: CARDINAL ;
   b      : ADDRESS ;
   p      : POINTER TO POINTER TO WORD ;
BEGIN
   WITH i^ DO
      IF NOT InBounds(i, n)
      THEN
         IF n<Low
         THEN
            HALT
         ELSE
            oldSize := ArraySize ;
            WHILE (n-Low)*TSIZE(ADDRESS)>=ArraySize DO
               ArraySize := ArraySize * GrowFactor
            END ;
            IF oldSize#ArraySize
            THEN
(*
               IF Debug
               THEN
                  printf2('increasing memory hunk from %d to %d\n',
                          oldSize, ArraySize)
               END ;
*)
               REALLOCATE(ArrayStart, ArraySize) ;
               (* and initialize the remainder of the array to NIL *)
               b := ArrayStart ;
               INC(b, oldSize) ;
               b := memset(b, 0, ArraySize-oldSize)
            END ;
            High := n
         END
      END ;
      b := ArrayStart ;
      INC(b, (n-Low)*TSIZE(ADDRESS)) ;
      p := b;
      p^ := a ;
      INC(Used) ;
      IF Debug
      THEN
         IF n<32
         THEN
            INCL(Map, n)
         END
      END
   END
END PutIndice ;


(*
   GetIndice - retrieves, element i[n] from the dynamic array.
*)

PROCEDURE GetIndice (i: Index; n: CARDINAL) : ADDRESS ;
VAR
   b: PtrToByte ;
   p: PtrToAddress ;
BEGIN
   WITH i^ DO
      IF NOT InBounds(i, n)
      THEN
         HALT
      END ;
      b := ArrayStart ;
      INC(b, (n-Low)*TSIZE(ADDRESS)) ;
      p := VAL(PtrToAddress, b) ;
      IF Debug
      THEN
         IF (n<32) AND (NOT (n IN Map)) AND (p^#NIL)
         THEN
            HALT
         END
      END ;
      RETURN( p^ )
   END
END GetIndice ;


(*
   IsIndiceInIndex - returns TRUE if, a, is in the index, i.
*)

PROCEDURE IsIndiceInIndex (i: Index; a: ADDRESS) : BOOLEAN ;
VAR
   j: CARDINAL ;
   b: PtrToByte ;
   p: PtrToAddress ;
BEGIN
   WITH i^ DO
      j := Low ;
      b := ArrayStart ;
      WHILE j<=High DO
         p := VAL(PtrToAddress, b) ;
         IF p^=a
         THEN
            RETURN( TRUE )
         END ;
         (* we must not INC(p, ..) as p2c gets confused *)
         INC(b, TSIZE(ADDRESS)) ;
         INC(j)
      END
   END ;
   RETURN( FALSE )
END IsIndiceInIndex ;


(*
   RemoveIndiceFromIndex - removes, a, from Index, i.
*)

PROCEDURE RemoveIndiceFromIndex (i: Index; a: ADDRESS) ;
VAR
   j: CARDINAL ;
   p: PtrToAddress ;
   b: PtrToByte ;
BEGIN
   WITH i^ DO
      j := Low ;
      b := ArrayStart ;
      WHILE j<=High DO
         p := VAL(PtrToAddress, b) ;
         INC(b, TSIZE(ADDRESS)) ;
         IF p^=a
         THEN
            DeleteIndice(i, j)
         END ;
         INC(j)
      END
   END
END RemoveIndiceFromIndex ;


(*
   DeleteIndice - delete i[j] from the array.
*)

PROCEDURE DeleteIndice (i: Index; j: CARDINAL) ;
VAR
   p: PtrToAddress ;
   b: PtrToByte ;
BEGIN
   WITH i^ DO
      IF InBounds(i, j)
      THEN
         b := ArrayStart ;
         INC(b, TSIZE(ADDRESS)*(j-Low)) ;
         p := VAL(PtrToAddress, b) ;
         INC(b, TSIZE(ADDRESS)) ;
         p := memmove(p, b, (High-j)*TSIZE(ADDRESS)) ;
         DEC(High) ;
         DEC(Used)
      ELSE
         HALT
      END
   END
END DeleteIndice ;


(*
   IncludeIndiceIntoIndex - if the indice is not in the index, then
                            add it at the end.
*)

PROCEDURE IncludeIndiceIntoIndex (i: Index; a: ADDRESS) ;
BEGIN
   IF NOT IsIndiceInIndex(i, a)
   THEN
      IF i^.Used=0
      THEN
         PutIndice(i, LowIndice(i), a)
      ELSE
         PutIndice(i, HighIndice(i)+1, a)
      END
   END
END IncludeIndiceIntoIndex ;


(*
   FindIndice - returns the indice containing a.
                It returns zero if a is not found in array i.
*)

PROCEDURE FindIndice (i: Index; a: ADDRESS) : CARDINAL ;
VAR
   j: CARDINAL ;
   p: PtrToAddress ;
   b: PtrToByte ;
BEGIN
   WITH i^ DO
      j := Low ;
      b := ArrayStart ;
      WHILE j <= High DO
         p := VAL (PtrToAddress, b) ;
         INC (b, TSIZE (ADDRESS)) ;
         IF p^ = a
         THEN
            RETURN j
         END ;
         INC (j)
      END
   END ;
   RETURN 0
END FindIndice ;


(*
   ForeachIndiceInIndexDo - for each j indice of i, call procedure p(i[j])
*)

PROCEDURE ForeachIndiceInIndexDo (i: Index; p: IndexProcedure) ;
VAR
   j: CARDINAL ;
BEGIN
   j := LowIndice(i) ;
   WHILE j<=HighIndice(i) DO
      p(GetIndice(i, j)) ;
      INC(j)
   END
END ForeachIndiceInIndexDo ;


(*
   IsEmpty - return TRUE if the array has no entries it.
*)

PROCEDURE IsEmpty (i: Index) : BOOLEAN ;
BEGIN
   RETURN i^.Used = 0
END IsEmpty ;


END Indexing.
