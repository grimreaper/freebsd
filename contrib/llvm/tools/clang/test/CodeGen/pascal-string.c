// RUN: clang-cc -emit-llvm -o - %s -fpascal-strings | grep "05Hello"

unsigned char * Foo( void )
{
  static unsigned char s[256] = "\pHello";
  return s;
}

