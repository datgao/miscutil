#!/usr/bin/env bash
r="/var/run/nmg.lck"

# Check prerequisites.
s() {
	[[ "$SUDO_USER" && "$#" == "2" && "$1" && "$2" ]] || return
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

# Restart panel applet as invoking user.
p() {
	x "${@:1:2}" bash -c 'killall nm-applet ; nohup nm-applet &>/dev/null &'
}

# Kill gnome daemons and restart panel.
k() {
	killall aptd fwupd snapd
	x "${@:1:2}" bash -c 'killall gnome-panel bamfdaemon gnome-software goa-daemon update-notifier ; kill `pgrep evolution` `pgrep tracker` `pgrep zeitgeist` ; nohup gnome-panel &>/dev/null &'
}

# Check user ID.
u="`id -u`" || exit
[[ "$u" == "0" ]] || exec sudo "$0" "$DISPLAY" "$DBUS_SESSION_BUS_ADDRESS" "$@" || exit

#  "$SESSION" "$XDG_MENU_PREFIX" "$WINDOWID" "$SESSION_MANAGER" "$QT_IM_MODULE" "$JOB" "$XMODIFIERS" "UBUNTU_MENUPROXY"

# Get environment from command line.
a=( "${@:1:2}" )

# Check dependencies.
s "${a[@]}" || exit

# Acquire lock to avoid multiple instances.
mkdir "$r" || exit

# Restart system service.
service network-manager restart

# Restart graphical component.
p "${a[@]}"

[[ "${3+"_"}" ]] && k "${a[@]}"

# Release lock.
rmdir "$r"
