// RUN: clang-cc -fsyntax-only -verify %s

@interface I
- (id) retain;
@end

void __raiseExc1() {
 [objc_lookUpClass("NSString") retain]; // expected-warning {{receiver type 'int' is not 'id'}} \
    expected-warning {{method '-retain' not found}}
}

typedef const struct __CFString * CFStringRef;

void func() {
  CFStringRef obj;

  [obj self]; // expected-warning {{receiver type 'CFStringRef' (aka 'struct __CFString const *') is not 'id'}} \\
                 expected-warning {{method '-self' not found}}
}
