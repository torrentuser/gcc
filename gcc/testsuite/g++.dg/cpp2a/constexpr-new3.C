// P0784R7
// { dg-do compile { target c++20 } }
// { dg-additional-options "-fdelete-null-pointer-checks" }

constexpr int *
f1 ()
{
  return new int (2);		// { dg-message "allocated here" }
}

constexpr auto v1 = f1 (); // { dg-error "is not a constant expression because it refers to a result of" }

constexpr bool
f2 ()
{
  int *p = new int (3);		// { dg-message "allocated here" }
  return false;
}

constexpr auto v2 = f2 (); // { dg-error "is not a constant expression because allocated storage has not been deallocated" }

constexpr bool
f3 ()
{
  int *p = new int (3);
  int *q = p;
  delete p;
  delete q;			// { dg-error "deallocation of already deallocated storage" }
  return false;
}

constexpr auto v3 = f3 ();	// { dg-message "in 'constexpr' expansion of" }

constexpr bool
f4 (int *p)
{
  delete p;			// { dg-error "destroying 'q' from outside current evaluation" }
  return false;
}

int q;
constexpr auto v4 = f4 (&q);	// { dg-message "in 'constexpr' expansion of" }

constexpr bool
f5 ()
{
  int *p = new int;		// { dg-message "allocated here" }
  return *p == 1;		// { dg-error "the content of uninitialized storage is not usable in a constant expression" }
}

constexpr auto v5 = f5 (); 	// { dg-message "in 'constexpr' expansion of" }

constexpr bool
f6 ()
{
  int *p = new int (2);		// { dg-message "allocated here" }
  int *q = p;
  delete p;
  return *q == 2;		// { dg-error "use of allocated storage after deallocation in a constant expression" }
}

constexpr auto v6 = f6 (); 	// { dg-message "in 'constexpr' expansion of" }

constexpr int *
f7 ()
{
  int *p = new int (2);		// { dg-message "allocated here" }
  delete p;
  return p;
}

constexpr auto v7 = f7 (); // { dg-error "is not a constant expression because it refers to a result of" }

constexpr bool
f8_impl (int *p)
{
  delete p;			// { dg-error "deallocation of storage that was not previously allocated" }
  return false;
}

constexpr bool
f8 ()
{
  int q = 0;
  return f8_impl (&q);
}
constexpr auto v8 = f8 ();	// { dg-message "in 'constexpr' expansion of" }
