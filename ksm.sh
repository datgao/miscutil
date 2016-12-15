#!/usr/bin/env bash
p="/sys/kernel/mm/ksm"
t="$1"
s="$2"
[[ "${t##[0-9]*}" ]] && t="15"
t="$((1000*(t)))"
[[ "${s##[0-9]*}" ]] && s="16384"
s="$((1048576/4096*(s)))"
if [[ -z "${1+"_"}" ]]
then
	grep -F '' "$p/"*
	exit
fi
r="$p/run"
[[ -e "$r" ]] || exit
m="`< "$r"`" || exit
[[ "$m" == "1" ]] && f="_"
[[ "$m" == "0" || "$f" ]] || exit
k=( "run" "sleep_millisecs" "pages_to_scan" )
[[ "$f" ]] && v=( "0" "20" "100" ) || v=( "1" "$t" "$s" )
for i in "${!k[@]}"
do
	d="$p/${k["$i"]}"
	n="${v["$i"]}"
	[[ "$d" && "$n" ]] || exit
	echo "$n" | tee "$d" || exit
done
