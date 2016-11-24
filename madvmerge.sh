#!/usr/bin/env bash
env LD_PRELOAD="`pwd`/libmadvmerge.so" "$@"
