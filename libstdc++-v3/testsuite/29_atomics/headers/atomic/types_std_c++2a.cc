// { dg-do compile { target c++20 } }

// Copyright (C) 2019-2025 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#include <atomic>

void test01()
{
  using std::memory_order;
  constexpr auto relaxed = memory_order::relaxed;
  constexpr auto consume = memory_order::consume;
  constexpr auto acquire = memory_order::acquire;
  constexpr auto release = memory_order::release;
  constexpr auto acq_rel = memory_order::acq_rel;
  constexpr auto seq_cst = memory_order::seq_cst;
}
