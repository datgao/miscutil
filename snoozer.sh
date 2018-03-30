#!/usr/bin/env bash
r="/var/run/snoozer.lck"

# Check prerequisites.
s() {
	local b p
	[[ "$SUDO_USER" && "$#" == "2" && "$1" && "$2" ]] || return
	b="`which zenity`" || return
	p="`which gnome-screensaver-command`" || return
	[[ "$b" && "$p" ]] || return
}

# Run graphical program as original user.
x() {
	local -a a
	local k
	a=( "$@" )
	for k in "${!a[@]}"
	do
		a[k]="`printf '%q' "${a[k]}"`"
	done
	su -c "env DISPLAY=${a[0]} DBUS_SESSION_BUS_ADDRESS=${a[1]} ${a[*]:2}" "$SUDO_USER"
}

# Display dialog window.
z() {
	x "${@:1:2}" zenity --question --text="Suspend?"
}

# Run g-s-c as invoking user.
g() {
	x "${@:1:2}" gnome-screensaver-command "${@:3}"
}

# Control backlight.
k() {
	x "${@:1:2}" toggle_backlight.sh "${@:3}"
}

# Lock session and sleep.  On resume, wait for user to unlock, and sleep again on timeout.
q() {
	local a c f h v w t
	w="$1"
	h="$2"
	a=( "${@:3}" )
	f="/sys/bus/iio/devices/iio:device0/in_illuminance_input"

	g "${a[@]}" --lock || return

	while true
	do
		[[ "$h" ]] || pm-suspend || echo "echo mem | tee /sys/power/state" || return
		t="$w"
		while [[ "$((t--))" -gt "0" ]]
		do
			[[ -r "$f" ]] && v="`< "$f"`" || v=""
			[[ "$v" && "$v" != "0" ]] && v="1"
			[[ "$v" ]] && k "${a[@]}" "$v"
			g "${a[@]}" --query || return
			g "${a[@]}" --query | grep -E '(^|[^a-z])active' || return
			sleep 1
		done
	done

}

# Check user ID.
u="`id -u`" || exit
[[ "$u" == "0" ]] || exec sudo "$0" "$DISPLAY" "$DBUS_SESSION_BUS_ADDRESS" "$@" || exit

# Get environment from command line.
a=( "${@:1:2}" )
f="$3"
h="${3+"_"}"
[[ "$f" ]] && h=""

# Sanitize wait in seconds.
[[ "${f##[0-9]}" || "${f%%*[0-9]}" ]] || w="${f##0*}"
[[ "$w" ]] || w="30"

# Check dependencies.
s "${a[@]}" || exit

# Confirm action.
[[ "$h" ]] || z "${a[@]}" || exit

# Acquire lock to avoid multiple instances.
mkdir "$r" || exit

# Run main loop.
q "$w" "$h" "${a[@]}"

# Release lock.
rmdir "$r"
