#!/usr/bin/env bash

# set -vETx

fnmain() {
	local -a a z
	local b c i f k m n p t v

	a=( "$@" )
	m="${#a[@]}"
	b=$'\r'
	[[ "$m" != "0" ]] && p="$(( m - 1 ))" && [[ "${a["$p"]}" == "" ]] && b="" && unset a["$((--m))"]
	z=( )
	for (( i = 0; i < m; i++ ))
	do
		t="${a["$i"]}"
		[[ -r "$t" ]] || return
		exec {f}<"$t" || return
		z+=( "$f" )
	done
	c="${#z[@]}"
	n="$c"

	while [[ "$n" != "0" ]]
	do
		m=""
		for (( i = 0; i < c; i++ ))
		do
			f="${z["$i"]}"
			! t="" || [[ "$f" == "" ]] || read -u "$f" t || ! t="" || ! ((n--)) || ! z["$i"]="" || exec {f}<&-
			p="${#t}" && [[ "$((p--))" != "0" ]] && [[ "${t:p}" == $'\r' ]] && t="${t:0:p}"
			printf '%s%s' "$m" "$t"
			m=$'\t'
		done
		printf '%s\n' "$b"
	done

	for k in "${!z[@]}"
	do
		f="${z["$k"]}"
		unset z["$k"]
		[[ "$f" ]] || continue
		exec {f}<&-
	done
}

fnmain "$@"
