#!/usr/bin/env bash
env LD_PRELOAD="`pwd`/libnocache.so" "$@"
