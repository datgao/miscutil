#!/usr/bin/env bash

fnmain() {
	local b c i n p REPLY

	read || return
	n="$REPLY"

	[[ "$n" == "${n/b}" ]] || b="1"
	[[ "$n" == "${n/c}" ]] || c="99"
	[[ "$n" == "${n/p}" ]] || p="1"
	n="${n//[bcp]}"

	echo 1 | sudo tee /sys/devices/system/cpu/cpu?/online || return
	echo powersave | sudo tee /sys/devices/system/cpu/cpu?/cpufreq/scaling_governor || return

	for(( i = 1; i <= 3; i++ ))
	do
		echo $(( (n >> (i-1)) & 1 )) | sudo tee /sys/devices/system/cpu/cpu$i/online || return
	done

	echo "${b:-"0"}" | sudo tee /sys/devices/system/cpu/cpufreq/boost || return

	echo "${c:-"0"}" | sudo tee /sys/class/thermal/cooling_device5/cur_state || return

	[[ "$p" == "" ]] || echo performance | sudo tee /sys/devices/system/cpu/cpu?/cpufreq/scaling_governor || return

	./w
}

fnmain "$@"
