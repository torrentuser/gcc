// Copyright (C) 2015-2025 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

// { dg-do run { target c++11 } }
// COW strings don't support C++11 allocator propagation:
// { dg-require-effective-target cxx11_abi }

#include <string>
#include <testsuite_hooks.h>
#include <testsuite_allocator.h>

using C = char;
const C c = 'a';
using traits = std::char_traits<C>;

using __gnu_test::uneq_allocator;

void test01()
{
  typedef uneq_allocator<C> alloc_type;
  typedef std::basic_string<C, traits, alloc_type> test_type;
  test_type v1(alloc_type(1));
  v1.assign(1, c);
  test_type v2(std::move(v1));
  VERIFY(1 == v1.get_allocator().get_personality());
  VERIFY(1 == v2.get_allocator().get_personality());
}

void test02()
{
  typedef uneq_allocator<C> alloc_type;
  typedef std::basic_string<C, traits, alloc_type> test_type;
  test_type v1(alloc_type(1));
  v1.assign(1, c);
  test_type v2(std::move(v1), alloc_type(2));
  VERIFY(1 == v1.get_allocator().get_personality());
  VERIFY(2 == v2.get_allocator().get_personality());
}

int main()
{
  test01();
  test02();
  return 0;
}
