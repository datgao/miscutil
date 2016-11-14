#!/usr/bin/env bash
p="/sys/class/backlight/intel_backlight"
b="$p/brightness"
m="$p/max_brightness"
[[ -e "$b" && -e "$m" ]] || exit

# Read current and maximum brightness.
n="`< "$p/max_brightness"`" || exit
v="`< "$p/brightness"`" || exit

# Toggle brightness.
echo "$((n-v))" | tee "$b"
