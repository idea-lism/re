#!/bin/sh
set -e
set -x

cd "$(dirname "$0")"

CC="${CC:-clang}"
CFLAGS="${CFLAGS:--std=c23 -O0 -g}"

$CC $CFLAGS -I ../out drive.c ../out/libre.a -o drive
./drive
$CC -c lex.ll -o lex.o
$CC $CFLAGS -I ../out main.c lex.o ../out/libre.a -o main
