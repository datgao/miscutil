#!/usr/bin/env bash
p="/sys/kernel/mm/ksm"
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
k=( "pages_to_scan" "sleep_millisecs" "run" )
[[ "$f" ]] && v=( "100" "20" "0" ) || v=( "4194304" "${1:-"15"}000" "1" )
for i in "${!k[@]}"
do
	d="$p/${k["$i"]}"
	n="${v["$i"]}"
	[[ "$d" && "$n" ]] || exit
	echo "$n" | tee "$d" || exit
done
