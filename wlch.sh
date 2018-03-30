#!/usr/bin/env bash
u="`id -u`" || exit
[[ "$u" == "0" ]] || exec sudo "$0" "$@" || exit

hpch() {
	local c f h m r w
	w="$1"
	m="$2"
	f="$3"
	c="$4"
	[[ "$c" == "" ]] && return
	r="/dev/null"

	if [[ "$c" == "+" ]]
	then
		ip link set "$w" down || return
		for h in 'HT20' ''
		do
			iw dev "$m" set freq "$f" $h &>"$r" && return
			r="/dev/stderr"
		done
	else
		iw dev "$w" offchannel "$f" 500
	fi
}

hpfr() {
	local c f m n p q w
	local -a a g
	w="$1"
	m="$2"
	c="$3"
	n="$4"
	p="5"
	q="$(( p + n ))"
	g=( "${@:p:n}" )
	a=( "${@:q}" )

	for f in "${a[@]}"
	do
		iw dev "$m" set monitor none || return
		hpch "$w" "$m" "$f" "$c" || return
		iw dev "$m" set monitor "${g[@]}" || return
		sleep 1
	done
}

main() {
	local c m n w
	local -a a g z

	a=( "$@" ) && n="${a[@]}" && [[ "$n" -le "3" ]] || return
	[[ "$n" == "0" ]] && ((n--))
	[[ "${a["$n"]}" ]] && c="+" || c="${a["$n"]+"_"}"
	unset a[n] && [[ "$n" == "0" ]] && ((n--))

	w="${a["$n"]}"
	if [[ "$w" == "" ]]
	then
		w="`ip link show | grep -oE 'wl(an|p)[0-9a-z_:\.\-]+:' | tail -1`" || exit
		w="${w%*:}" && [[ "$w" ]] || exit
	fi
	unset a[n] && [[ "$n" == "0" ]] && ((n--))

	m="${a["$n"]}"
	if [[ "$m" == "" ]]
	then
		m="`ip link show | grep -oE 'moni?[0-9a-z_:\.\-]+:' | tail -1`" || exit
		m="${m%*:}" && [[ "$m" ]] || m="mon0"
	fi

	g=( "otherbss" "control" )
	rfkill unblock wifi || return
	ip link show dev "$m" &>/dev/null || iw dev "$w" interface add "$m" type monitor flags "${g[@]}" || return
	ip link set dev "$m" up || return
	[[ "$c" == "+" ]] || ip link set "$w" up || return

	while true
	do
		hpfr "$w" "$m" "$c" "${#g[@]}" "${g[@]}" 2412 2437 2462 || return
	done
}

main "$@"
