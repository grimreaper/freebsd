// RUN: clang-cc -fsyntax-only -verify %s 
class X { };

X operator+(X, X);

void f(X x) {
  x = x + x;
}

struct Y;
struct Z;

struct Y {
  Y(const Z&);
};

struct Z {
  Z(const Y&);
};

Y operator+(Y, Y);
bool operator-(Y, Y); // expected-note{{candidate function}}
bool operator-(Z, Z); // expected-note{{candidate function}}

void g(Y y, Z z) {
  y = y + z;
  bool b = y - z; // expected-error{{use of overloaded operator '-' is ambiguous; candidates are:}}
}

struct A {
  bool operator==(Z&); // expected-note 2{{candidate function}}
};

A make_A();

bool operator==(A&, Z&); // expected-note 2{{candidate function}}

void h(A a, const A ac, Z z) {
  make_A() == z;
  a == z; // expected-error{{use of overloaded operator '==' is ambiguous; candidates are:}}
  ac == z; // expected-error{{invalid operands to binary expression ('struct A const' and 'struct Z')}}
}

struct B {
  bool operator==(const B&) const;

  void test(Z z) {
    make_A() == z;
  }
};

enum Enum1 { };
enum Enum2 { };

struct E1 {
  E1(Enum1) { }
};

struct E2 {
  E2(Enum2);
};

// C++ [over.match.oper]p3 - enum restriction.
float& operator==(E1, E2); 

void enum_test(Enum1 enum1, Enum2 enum2, E1 e1, E2 e2) {
  float &f1 = (e1 == e2);
  float &f2 = (enum1 == e2); 
  float &f3 = (e1 == enum2); 
  float &f4 = (enum1 == enum2);  // expected-error{{non-const lvalue reference to type 'float' cannot be initialized with a temporary of type 'bool'}}
}


struct PostInc {
  PostInc operator++(int);
  PostInc& operator++();
};

struct PostDec {
  PostDec operator--(int);
  PostDec& operator--();
};

void incdec_test(PostInc pi, PostDec pd) {
  const PostInc& pi1 = pi++;
  const PostDec& pd1 = pd--;
  PostInc &pi2 = ++pi;
  PostDec &pd2 = --pd;
}

struct SmartPtr {
  int& operator*();
  long& operator*() const volatile;
};

void test_smartptr(SmartPtr ptr, const SmartPtr cptr, 
                   const volatile SmartPtr cvptr) {
  int &ir = *ptr;
  long &lr = *cptr;
  long &lr2 = *cvptr;
}


struct ArrayLike {
  int& operator[](int);
};

void test_arraylike(ArrayLike a) {
  int& ir = a[17];
}

struct SmartRef {
  int* operator&();
};

void test_smartref(SmartRef r) {
  int* ip = &r;
}

bool& operator,(X, Y);

void test_comma(X x, Y y) {
  bool& b1 = (x, y);
  X& xr = (x, x);
}

struct Callable {
  int& operator()(int, double = 2.71828); // expected-note{{candidate function}}
  float& operator()(int, double, long, ...); // expected-note{{candidate function}}

  double& operator()(float); // expected-note{{candidate function}}
};

struct Callable2 {
  int& operator()(int i = 0);
  double& operator()(...) const;
};

void test_callable(Callable c, Callable2 c2, const Callable2& c2c) {
  int &ir = c(1);
  float &fr = c(1, 3.14159, 17, 42);

  c(); // expected-error{{no matching function for call to object of type 'struct Callable'; candidates are:}}

  double &dr = c(1.0f);

  int &ir2 = c2();
  int &ir3 = c2(1);
  double &fr2 = c2c();
}

typedef float FLOAT;
typedef int& INTREF;
typedef INTREF Func1(FLOAT, double);
typedef float& Func2(int, double);

struct ConvertToFunc {
  operator Func1*(); // expected-note{{conversion candidate of type 'INTREF (*)(FLOAT, double)'}}
  operator Func2&(); // expected-note{{conversion candidate of type 'float &(&)(int, double)'}}
  void operator()();
};

void test_funcptr_call(ConvertToFunc ctf) {
  int &i1 = ctf(1.0f, 2.0);
  float &f2 = ctf((short int)1, 1.0f);
  ctf((long int)17, 2.0); // expected-error{{error: call to object of type 'struct ConvertToFunc' is ambiguous; candidates are:}}
  ctf();
}

struct HasMember {
  int m;
};

struct Arrow1 {
  HasMember* operator->();
};

struct Arrow2 {
  Arrow1 operator->(); // expected-note{{candidate function}}
};

void test_arrow(Arrow1 a1, Arrow2 a2, const Arrow2 a3) {
  int &i1 = a1->m;
  int &i2 = a2->m;
  a3->m; // expected-error{{no viable overloaded 'operator->'; candidate is}}
}

struct CopyConBase {
};

struct CopyCon : public CopyConBase {
  CopyCon(const CopyConBase &Base);

  CopyCon(const CopyConBase *Base) {
    *this = *Base;
  }
};

namespace N {
  struct X { };
}

namespace M {
  N::X operator+(N::X, N::X);
}

namespace M {
  void test_X(N::X x) {
    (void)(x + x);
  }
}

struct AA { bool operator!=(AA&); };
struct BB : AA {};
bool x(BB y, BB z) { return y != z; }


struct AX { 
  AX& operator ->();	 // expected-note {{declared at}}
  int b;
}; 

void m() {
  AX a; 
  a->b = 0; // expected-error {{circular pointer delegation detected}}
}

struct CircA {
  struct CircB& operator->(); // expected-note {{declared at}}
  int val;
};
struct CircB {
  struct CircC& operator->(); // expected-note {{declared at}}
};
struct CircC {
  struct CircA& operator->(); // expected-note {{declared at}}
};

void circ() {
  CircA a;
  a->val = 0; // expected-error {{circular pointer delegation detected}}
}
