#!/bin/sh

set -e

CFLAGS="-O3 -march=native -pipe -flto=auto -fno-inline -g" make -j"$(nproc)"
