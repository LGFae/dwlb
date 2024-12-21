#!/bin/sh

set -e

CFLAGS="-O3 -march=native -pipe -flto=auto -g" make -j"$(nproc)"
