// Test the -fwritable-strings option.

// RUN: %llvmgcc -O3 -S -o - -emit-llvm -fwritable-strings %s | \
// RUN:    grep {private global}
// RUN: %llvmgcc -O3 -S -o - -emit-llvm %s | grep {private constant}

char *X = "foo";
