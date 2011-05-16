#! /bin/sh

exec ./configure "$@" --disable-cpp --enable-utf8 --enable-unicode-properties --disable-shared CFLAGS="-O2 -g"
