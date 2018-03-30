#!/usr/bin/env bash
a=( "$@" )
i="0"
m="${a[i]}"
[[ "${m:0:2}" == "--" ]] && unset a[i++] || m=""

r="${a[i]}"
unset a[i++]
[[ "$r" ]] || exit

cd "$r" && \
for d in "${a[@]}"
do
	find "$d" -name . -o -name .. -o \( \! -type d -o -empty \) -print
done \
| rev | sort | rev \
| tar -T - -cf - \
| pv -ptrabfWcN tar -B 1048576 -s "$((1024*`cd "$r" && du -ks "${a[@]}" | awk '/^[[:digit:]]+/ { s+=$1 }  END { print s }'`))" \
| time xz -cf ${m:+"$m"} --best --extreme \
| pv -ptrabfWcN xz -B 1048576
