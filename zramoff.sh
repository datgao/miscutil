#!/usr/bin/env bash
cat /proc/swaps
for d in /dev/zram*
do
	[[ -e "$d" ]] || break
	time swapoff "$d" || [[ "${1+"_"}" ]] || exit
done
service zram-config restart
