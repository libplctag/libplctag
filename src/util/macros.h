

#ifndef __MACROS_H__
#define __MACROS_H__


#include <stddef.h>

/*
 * This is a collection of handy macros.
 */

/* select one out of a list */
#define GET_MACRO(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,NAME,...) NAME

#define FOREACH_PAIR_2(WHAT, WHAT_LAST, X, Y) WHAT_LAST(X,Y)
#define FOREACH_PAIR_4(WHAT, WHAT_LAST, X, Y, ...) WHAT(X,Y)FOREACH_PAIR_2(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_PAIR_6(WHAT, WHAT_LAST, X, Y, ...) WHAT(X,Y)FOREACH_PAIR_4(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_PAIR_8(WHAT, WHAT_LAST, X, Y, ...) WHAT(X,Y)FOREACH_PAIR_6(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_PAIR_10(WHAT, WHAT_LAST, X, Y, ...) WHAT(X,Y)FOREACH_PAIR_8(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_PAIR_12(WHAT, WHAT_LAST, X, Y, ...) WHAT(X,Y)FOREACH_PAIR_10(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_PAIR_14(WHAT, WHAT_LAST, X, Y, ...) WHAT(X,Y)FOREACH_PAIR_12(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_PAIR_16(WHAT, WHAT_LAST, X, Y, ...) WHAT(X,Y)FOREACH_PAIR_14(WHAT, WHAT_LAST, __VA_ARGS__)

/* run an action macro against all elements passed as arguments, handles arguments in PAIRS */
#define FOREACH_PAIR(action, ...) \
  GET_MACRO(__VA_ARGS__,FOREACH_PAIR_16,OOPS15,FOREACH_PAIR_14,OOPS13,FOREACH_PAIR_12,OOPS11,FOREACH_PAIR_10,OOPS9,FOREACH_PAIR_8,OOPS7,FOREACH_PAIR_6,OOPS5,FOREACH_PAIR_4,OOPS3,FOREACH_PAIR_2,OOPS1,)(action,action,__VA_ARGS__)

/* as above, but run a second macro function against the last element in the list */
#define FOREACH_PAIR_LAST(action, action_last, ...) \
  GET_MACRO(__VA_ARGS__,FOREACH_PAIR_16,OOPS15,FOREACH_PAIR_14,OOPS13,FOREACH_PAIR_12,OOPS11,FOREACH_PAIR_10,OOPS9,FOREACH_PAIR_8,OOPS7,FOREACH_PAIR_6,OOPS5,FOREACH_PAIR_4,OOPS3,FOREACH_PAIR_2,OOPS1,)(action,action_last,__VA_ARGS__)


#define FOREACH_1(WHAT, WHAT_LAST, X) WHAT_LAST(X)
#define FOREACH_2(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_1(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_3(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_2(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_4(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_3(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_5(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_4(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_6(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_5(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_7(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_6(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_8(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_7(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_9(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_8(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_10(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_9(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_11(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_10(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_12(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_11(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_13(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_12(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_14(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_13(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_15(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_14(WHAT, WHAT_LAST, __VA_ARGS__)
#define FOREACH_16(WHAT, WHAT_LAST, X, ...) WHAT(X)FOREACH_15(WHAT, WHAT_LAST, __VA_ARGS__)


/* run action macro against all elements in a list. */
#define FOREACH(action, ...) \
   GET_MACRO(__VA_ARGS__,FOREACH_16,FOREACH_15,FOREACH_14,FOREACH_13,FOREACH_12,FOREACH_11,FOREACH_10,FOREACH_9,FOREACH_8,FOREACH_7,FOREACH_6,FOREACH_5,FOREACH_4,FOREACH_3,FOREACH_2,FOREACH_1,)(action,action,__VA_ARGS__)

/* run action macro against all elements in a list. Run a different macro against the last element. */
#define FOREACH_LAST(action, action_last, ...) \
   GET_MACRO(__VA_ARGS__,FOREACH_16,FOREACH_15,FOREACH_14,FOREACH_13,FOREACH_12,FOREACH_11,FOREACH_10,FOREACH_9,FOREACH_8,FOREACH_7,FOREACH_6,FOREACH_5,FOREACH_4,FOREACH_3,FOREACH_2,FOREACH_1,)(action,action_last,__VA_ARGS__)

/* count args */
#define COUNT_NARG(...)                                                \
         COUNT_NARG_(__VA_ARGS__,COUNT_RSEQ_N())

#define COUNT_NARG_(...)                                               \
         COUNT_ARG_N(__VA_ARGS__)

#define COUNT_ARG_N(                                                   \
          _1, _2, _3, _4, _5, _6, _7, _8, _9,_10, \
         _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
         _21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
         _31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
         _41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
         _51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
         _61,_62,_63,N,...) N

#define COUNT_RSEQ_N()                                                 \
         63,62,61,60,                   \
         59,58,57,56,55,54,53,52,51,50, \
         49,48,47,46,45,44,43,42,41,40, \
         39,38,37,36,35,34,33,32,31,30, \
         29,28,27,26,25,24,23,22,21,20, \
         19,18,17,16,15,14,13,12,11,10, \
         9,8,7,6,5,4,3,2,1,0


#define container_of(ptr, type, member) ((type *)((char *)(1 ? (ptr) : &((type *)0)->member) - offsetof(type, member)))


#endif
