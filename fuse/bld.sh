#!/bin/sh
cc -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -lfuse -pthread examples/hello.c -DFUSE_USE_VERSION=27 -o examples/a.out -g3
