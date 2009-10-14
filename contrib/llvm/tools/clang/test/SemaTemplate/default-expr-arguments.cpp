// RUN: clang-cc -fsyntax-only -verify %s

template<typename T>
class C { C(int a0 = 0); };

template<>
C<char>::C(int a0);

struct S { };

template<typename T> void f1(T a, T b = 10) { } // expected-error{{cannot initialize 'b' with an rvalue of type 'int'}}

template<typename T> void f2(T a, T b = T()) { }

template<typename T> void f3(T a, T b = T() + T()); // expected-error{{invalid operands to binary expression ('struct S' and 'struct S')}}

void g() {
  f1(10);
  f1(S()); // expected-note{{in instantiation of default function argument expression for 'f1<struct S>' required here}}
  
  f2(10);
  f2(S());
  
  f3(10);
  f3(S()); // expected-note{{in instantiation of default function argument expression for 'f3<struct S>' required here}}
}

template<typename T> struct F {
  F(T t = 10); // expected-error{{cannot initialize 't' with an rvalue of type 'int'}}
  void f(T t = 10); // expected-error{{cannot initialize 't' with an rvalue of type 'int'}}
};

struct FD : F<int> { };

void g2() {
  F<int> f;
  FD fd;
}

void g3(F<int> f, F<struct S> s) {
  f.f();
  s.f(); // expected-note{{in instantiation of default function argument expression for 'f<struct S>' required here}}
  
  F<int> f2;
  F<S> s2; // expected-note{{in instantiation of default function argument expression for 'F<struct S>' required here}}
}

template<typename T> struct G {
  G(T) {}
};

void s(G<int> flags = 10) { }

// Test default arguments
template<typename T>
struct X0 {
  void f(T = T()); // expected-error{{no matching}}
};

template<typename U>
void X0<U>::f(U) { }

void test_x0(X0<int> xi) {
  xi.f();
  xi.f(17);
}

struct NotDefaultConstructible { // expected-note{{candidate}}
  NotDefaultConstructible(int); // expected-note{{candidate}}
};

void test_x0_not_default_constructible(X0<NotDefaultConstructible> xn) {
  xn.f(NotDefaultConstructible(17));
  xn.f(42);
  xn.f(); // expected-note{{in instantiation of default function argument}}
}

template<typename T>
struct X1 {
  typedef T value_type;
  X1(const value_type& value = value_type());
};

void test_X1() {
  X1<int> x1;
}
