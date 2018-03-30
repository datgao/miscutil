#!/usr/bin/env bash
w="3600"
n="3"
d="5"

u="`id -u`" || exit
[[ "$u" == "0" ]] || exec sudo env DISPLAY="$DISPLAY" DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" "$0" "$@" || exit
# [[ "$u" == "0" ]] || exec time sudo "$0" "$@" || exit

s="/sys/power/state"
p="/sys/class/power_supply/AC/online"
[[ -e "$p" ]] || exit

g=( "gnome-screensaver-command" "--lock" )
[[ "$SUDO_USER" ]] && g=( "su" "-c" "env DISPLAY='$DISPLAY' DBUS_SESSION_BUS_ADDRESS='$DBUS_SESSION_BUS_ADDRESS' ${g[*]}" "$SUDO_USER" )

rtcwaker() {
	which rtcwake &>/dev/null && rtcwake -u "$@"
}

rtcalarm() {
	rtcwaker -m show
}

rtcsleep() {
	local r
	rtcalarm || r="$?"
	rtcwaker -m disable || r="$?"
	return "${r:-"0"}"
}

rtcsleep

while true
do
	i="$n"

	while true
	do
		sleep "$d"
		a="`< "$p"`" || exit
		[[ "$a" == "1" ]] && break
		[[ "$a" == "0" ]] || exit
		[[ "$((--i))" == "0" ]] && break
	done
	[[ "$a" == "1" ]] && continue

	rtcalarm
	rtcwaker -u -m no -s "$w"

	"${g[@]}"
	echo -n "Sleeping at: " && date -u

	pm-suspend || echo mem | tee "$s" || exit

	echo -n "Resuming at: " && date -u
	rtcsleep

	xset dpms force off
done
