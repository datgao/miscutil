#!/usr/bin/env bash
u="`id -u`" || exit
[[ "$u" == "0" ]] || exec sudo "$0" "$@" || exit

cat /proc/swaps

for d in /dev/zram*
do
	[[ -e "$d" ]] || break
	time swapoff "$d" || [[ "${1+"_"}" ]] || exit
done

service zram-config restart
