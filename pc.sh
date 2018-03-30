#!/usr/bin/env bash
d="$1"
a=( "${@:2}" )
[[ "$d" ]] || exit
tar cf - "${a[@]}" \
| time pv -ptrabfeWB 1048576 -s "$((1024*`du -ks "${a[@]}" | awk '/^[[:digit:]]+/ { s+=$1 }  END { print s }'`))" \
| time tar -C "$d" -xvf -
# time pv -ptrabfeWB 1048576 -s "`stat -c %s "$f"`"
