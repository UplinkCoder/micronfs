#!/bin/sh
cc -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -lfuse -pthread cnfs_main.c ../cache/cached_tree.c micronfs_glue.c ../micronfs.c ../rpc_serializer.c -DFUSE_USE_VERSION=27 -o a.out -g3
