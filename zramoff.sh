#!/usr/bin/env bash
for d in /dev/zram*
do
	time swapoff "$d" || exit
done
service zram-config restart
