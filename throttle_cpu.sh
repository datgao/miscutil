#!/usr/bin/env bash

# TODO: man pm-powersave

y="/sys/devices/system/cpu"
q="cpufreq/scaling_governor"
z="$y/cpu0/$q"
c="$y/cpu?"
v="$y/cpufreq/boost"
p="powersave"

# Watch temperature if parameter is set.
k="${1+"_"}" && o=""

# Read selected governor.
[[ -f "$z" ]] && g="`< "$z"`" || exit
a="$g"

# Find path for intel-powerclamp module, if present (no associated device path).
for d in /sys/class/thermal/cooling_device*
do
	[[ -e "$d/device" ]] && continue
	t="$d" && break
done
[[ "$t" ]] && u="$t/cur_state" && m="$t/max_state"

while true
do
[[ "$k" ]] && sleep 1

w="-" && d="30000" && t="15000"
for h in /sys/class/hwmon/hwmon*/temp*_max
do
[[ "$k" ]] || break
l="${h:0:$((${#h}-4))}_input"
[[ -e "$h" && -e "$l" ]] || continue
i="`< "$h"`" && j="`< "$l"`" || exit
[[ "$((j+t))" -ge "$i" ]] && w="+" && break
[[ "$((j+d))" -ge "$i" ]] && w="_"
done
[[ "$w" == "$o" ]] && h="0" || h="1"
[[ "$k" ]] || h=""
[[ "$w" == "_" ]] || o="$w"
[[ "$h" == "0" || "$w" == "_" ]] && continue

# Set flag if powersave.
[[ "$g" == "$p" ]] && f="_" || f=""

# Expand all CPU online nodes, read state, and enable by default.
e=( $c/online ) && x="${e[0]}" && r="1"
[[ -f "$x" ]] && x="`< "$x"`" || exit
[[ "$x" == "1" ]] || tee "${e[@]}" <<< "$r" || exit

# Read selected governor.
[[ "$h" == "1" && "$w" == "+" ]] && d="_" || d=""
[[ -z "$d" ]] || g="`< "$z"`" || exit
[[ -z "$d" ]] || a="$g"

# Default powersave governor, disable turbo boost, and no scheduled throttling.
s="$p" && b="0" && n="0"

# If powersave governor and online, offline non-boot CPUs, and set max throttle.
[[ "$d" || -z "$k" && "$f" && "$x" == "1" ]] && r="0" && n=""

# If powersave governor and offline, set ondemand governor and enable turbo boost.
[[ "$h" == "1" && "$w" == "-" && "$a" != "$p" || -z "$k" && "$f" && "$x" == "0" ]] && b="1" && s="ondemand"

# Apply turbo boost setting.
[[ ! -f "$v" ]] || tee "$v" <<< "$b" || exit

# Set CPU scaling governor.
[[ "$g" == "$s" ]] || tee $c/$q <<< "$s" || exit
g="$s"

# Update online CPUs.
[[ "$r" == "1" ]] || tee "${e[@]}" <<< "$r" || exit

# Configure throttle tunable.  This forcibly schedules idle time slices as kidle_inject/* threads.
[[ "$n" || ! -f "$m" ]] || n="`< "$m"`" || exit
[[ -z "$n" || ! -f "$u" ]] || tee "$u" <<< "$n" || exit

[[ "$k" ]] || break
done
