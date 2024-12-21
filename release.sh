#!/bin/sh

set -e

CFLAGS="-O3 -march=native -pipe -flto=auto" make -j"$(nproc)"
