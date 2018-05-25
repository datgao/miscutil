#!/usr/bin/env bash
f="libmadvmerge.so"

p="$0"
d="${p%/*}"
[[ "$d" == "$p" ]] && d="" || d="$d/"

p="`pwd`" && [[ "${p:0:1}" == "/" ]] || exit
[[ "${d:0:1}" == "/" && "${#d}" -gt 1 ]] && t="$d" || t="$p/$d"

e="$t$f"
[[ -e "$e" ]] && e="LD_PRELOAD=${LD_PRELOAD:+"$LD_PRELOAD:"}$e" || e=""

env ${e:+"$e"} "$@"
