#!/usr/bin/env bash
r="/var/run/snoozer.lck"

# Run g-s-c as invoking user.
g() {
	[[ "$SUDO_USER" ]] || return
	su -c "env DISPLAY='$2' DBUS_SESSION_BUS_ADDRESS='$3' gnome-screensaver-command '$1'" "$SUDO_USER"
}

q() {
	local a c s t
	s="$1"
	a=( "${@:2}" )

	g --lock "${a[@]}" || return

	while true
	do
		pm-suspend || echo mem | tee /sys/power/state || return
		t="$s"
		while [[ "$((t--))" -gt "0" ]]
		do
			g --query "${a[@]}" || return
			g --query "${a[@]}" | grep -E '(^|[^a-z])active' || return
			sleep 1
		done
	done

}

# Acquire lock to avoid multiple instances.
mkdir "$r" || exit

# Sanitize wait in seconds.
s="$1"
s="${s##[0-9]*}"
[[ "$s" && "$s" -ge "10" ]] || s="30"

# Lock session and sleep.  On resume, wait for user to unlock, and sleep again on timeout.
q "$s" "${@:2}"

# Release lock.
rmdir "$r"
