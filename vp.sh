#!/usr/bin/env bash
b="65536"
c=( )
a=( "$@" )
for s in "${a[@]}"
do
	[[ "$s" == "" && "${s:+"_"}" ]] && b="" && continue
done
pv -ptrabfeW "$@" ${b:+"-B" "$b"} 2> >(tr '\r' '\n' >&2)
