#!/bin/sh

set -e

CFLAGS="-Og -g3 -fsanitize=undefined,address" make -j"$(nproc)"
