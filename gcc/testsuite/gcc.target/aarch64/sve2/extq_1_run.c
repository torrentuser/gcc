/* { dg-do run { target { aarch64_sve256_hw && aarch64_sve2p1_hw } } } */
/* { dg-options "-O2 -msve-vector-bits=256" } */

#include "extq_1.c"

#define TEST(A, B)							\
  do {									\
    typeof(B) actual_ = (A);						\
    if (__builtin_memcmp (&actual_, &(B), sizeof (actual_)) != 0)	\
      __builtin_abort ();						\
  } while (0)

int
main ()
{
  fixed_float64_t a64 = { 1.5, 3.75, -5.25, 9 };
  fixed_float64_t b64 = { -2, 4.125, -6.375, 11.5 };
  fixed_float64_t expected1 = { 3.75, -2, 9, -6.375 };
  TEST (f1 (a64, b64), expected1);

  fixed_uint32_t a32 = { 0x1122, -0x3344, 0x5566, -0x7788,
			 0x99aa, -0xbbcc, 0xddee, -0xff00 };
  fixed_uint32_t b32 = { 1 << 20, 1 << 21, 1 << 22, 1 << 23,
			 5 << 6, 5 << 7, 5 << 8, 5 << 9 };
  fixed_uint32_t expected2 = { -0x3344, 0x5566, -0x7788, 1 << 20,
			       -0xbbcc, 0xddee, -0xff00, 5 << 6 };
  fixed_uint32_t expected3 = { -0x7788, 1 << 20, 1 << 21, 1 << 22,
			       -0xff00, 5 << 6, 5 << 7, 5 << 8 };
  TEST (f2 (a32, b32), expected2);
  TEST (f3 (a32, b32), expected3);

  fixed_float16_t a16 = { 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.25,
			  2.5, 2.75, 3, 3.25, 3.5, 3.75, 4, 4.25 };
  fixed_float16_t b16 = { -0.5, -0.75, -1, -1.25, -1.5, -1.75, -2, -2.25,
			  -2.5, -2.75, -3, -3.25, -3.5, -3.75, -4, -4.25 };
  fixed_float16_t expected4 = { 0.75, 1, 1.25, 1.5, 1.75, 2, 2.25, -0.5,
				2.75, 3, 3.25, 3.5, 3.75, 4, 4.25, -2.5 };
  fixed_float16_t expected5 = { 1.75, 2, 2.25, -0.5, -0.75, -1, -1.25, -1.5,
				3.75, 4, 4.25, -2.5, -2.75, -3, -3.25, -3.5 };
  fixed_float16_t expected6 = { 2.25, -0.5, -0.75, -1,
				-1.25, -1.5, -1.75, -2,
				4.25, -2.5, -2.75, -3,
				-3.25, -3.5, -3.75, -4 };
  TEST (f4 (a16, b16), expected4);
  TEST (f5 (a16, b16), expected5);
  TEST (f6 (a16, b16), expected6);

  fixed_int8_t a8 = { 0x01, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x70,
		      0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf8,
		      0xfe, 0xed, 0xdc, 0xcb, 0xba, 0xa9, 0x98, 0x8f,
		      0x76, 0x65, 0x54, 0x43, 0x32, 0x21, 0x10, 0x07 };
  fixed_int8_t b8 = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
		      0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
		      0x13, 0x24, 0x35, 0x46, 0x57, 0x68, 0x79, 0x8a,
		      0x9b, 0xac, 0xbd, 0xce, 0xdf, 0xe0, 0xf1, 0x02 };
  fixed_int8_t expected7 = { 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x70, 0x89,
			     0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf8, 0x11,
			     0xed, 0xdc, 0xcb, 0xba, 0xa9, 0x98, 0x8f, 0x76,
			     0x65, 0x54, 0x43, 0x32, 0x21, 0x10, 0x07, 0x13 };
  fixed_int8_t expected8 = { 0xbc, 0xcd, 0xde, 0xef, 0xf8, 0x11, 0x22, 0x33,
			     0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
			     0x43, 0x32, 0x21, 0x10, 0x07, 0x13, 0x24, 0x35,
			     0x46, 0x57, 0x68, 0x79, 0x8a, 0x9b, 0xac, 0xbd };
  fixed_int8_t expected9 = { 0xf8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
			     0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
			     0x07, 0x13, 0x24, 0x35, 0x46, 0x57, 0x68, 0x79,
			     0x8a, 0x9b, 0xac, 0xbd, 0xce, 0xdf, 0xe0, 0xf1 };
  TEST (f7 (a8, b8), expected7);
  TEST (f8 (a8, b8), expected8);
  TEST (f9 (a8, b8), expected9);

  return 0;
}
