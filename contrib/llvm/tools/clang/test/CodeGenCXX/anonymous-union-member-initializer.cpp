// RUN: %clang_cc1 -emit-llvm -o - %s

struct A {
  union {
    int a;
    void* b;
  };
  
  A() : a(0) { }
};

A a;
