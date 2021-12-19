#!/bin/sh
set -ve
autoconf
(cd pcre; autoconf)
./configure
make

