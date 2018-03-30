#!/usr/bin/env bash
# grep '' /sys/class/hwmon/hwmon0/*
(
for d in /sys/class/hwmon/hwmon*
do
	cd "$d" 2>/dev/null || continue

	m=""
	for f in temp?_max
	do
		[[ -f "$f" ]] && s="`< "$f"`" && [[ "${s%0}" && "${s%0}" && "${s%000}" ]] && m="1" && break
	done

	c=""
	for f in temp?_crit
	do
		[[ -f "$f" ]] && s="`< "$f"`" && [[ "${s%0}" && "${s%0}" && "${s%000}" ]] && c="1" && break
	done

	r=""
	for f in temp?_crit_alarm
	do
		[[ -f "$f" ]] && s="`< "$f"`" && [[ "${s%0}" && "${s%0}" && "${s%000}" ]] && r="1" && break
	done

	cat "name" 2>/dev/null | tr -d '\n'
	echo
	printf '\t%s' "label" "input" ${m:+"max"} ${c:+"crit"} ${r:+"crit_alarm"}
	echo

	for f in temp?_input
	do
		b="${f%_input}"
		t="${b}_label"
		printf '\t'
		[[ -f "$t" ]] && cat "$t" 2>/dev/null | tr -d '\n' || printf '%s' "[${b#temp}]"

		for t in "$f" ${m:+"${b}_max"} ${c:+"${b}_crit"} ${r:+"${b}_crit_alarm"}
		do
			printf '\t'
			[[ -f "$t" ]] && s="`< "$t"`" && [[ "${s%0}" && "${s%0}" && "${s%000}" ]] || continue
			n="${#s}"
			[[ "$n" -le "3" ]] && printf '(%s)' "$s" && continue
			p="$((n-3))"
			s="${s:0:p}.${s:p}"
			for g in 1 2 3
			do
				s="${s%0}"
			done
			printf '%s' "${s%.}"
		done
		echo
	done
done
)
