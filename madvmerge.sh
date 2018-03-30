#!/usr/bin/env bash
b="libmadvmerge.so"
t="$0"
d="${t%/*}"
[[ "$t" != "$d" && -d "$d" ]] && p="$d/$b" && [[ -e "$p" ]] || ! p="`pwd`/$b" || [[ -e "$p" ]] || exit
env LD_PRELOAD="$p" "$@"
