#!/usr/bin/env bash
y="/sys/devices/system/cpu"
q="cpufreq/scaling_governor"
z="$y/cpu0/$q"
c="$y/cpu?"
v="$y/cpufreq/boost"
p="powersave"

# Read selected governor.
[[ -f "$z" ]] && g="`< "$z"`" || exit

# Set flag if powersave.
[[ "$g" == "$p" ]] && f="_"

# Expand all CPU online nodes, read state, and enable by default.
e="$c/online"
e=( $e )
x="${e[0]}"
[[ -f "$x" ]] && x="`< "${e[0]}"`" || exit
r="1"
tee "${e[@]}" <<< "$r"

# Find path for intel-powerclamp module, if present (no associated device path).
for d in /sys/class/thermal/cooling_device*
do
	[[ -e "$d/device" ]] && continue
	t="$d" && break
done
[[ "$t" ]] && m="$t/max_state" && u="$t/cur_state"

# Default powersave governor, disable turbo boost, and no scheduled throttling.
s="$p"
b="0"
n="0"

# If powersave governor and online, offline non-boot CPUs, and set max throttle.
[[ "$f" && "$x" == "1" ]] && r="0" && [[ "$m" && -f "$m" ]] && n="`< "$m"`"

# If powersave governor and offline, set ondemand governor and enable turbo boost.
[[ "$f" && "$x" == "0" ]] && b="1" && s="ondemand"

# Apply turbo boost setting.
[[ -f "$v" ]] && tee "$v" <<< "$b"

# Set CPU scaling governor.
a="$c/$q"
a=( $a )
tee "${a[@]}" <<< "$s"

# Update online CPUs.
tee "${e[@]}" <<< "$r"

# Configure throttle tunable.  This forcibly schedules idle time slices as kidle_inject/* threads.
[[ -f "$u" ]] && tee "$u" <<< "$n"

# Return early if parameter is set.
# [[ "${1+"_"}" ]] && exit
