#!/usr/bin/env bash
r="/var/run/backlighter.lck"

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

# Turn off display as invoking user.
p() {
	x "${@:1:2}" xset dpms force off
}

k() {
	local b h i m n p v
	local -a a
	h="$1"
	a=( "${@:2}" )
	p="/sys/class/backlight/intel_backlight"
	b="$p/brightness"
	m="$p/max_brightness"

	# '-' = cycle brightness and off, '_' = turn off only, '+' = toggle backlight only, '0' = dim, '1' = bright

	if [[ "$h" != "_" ]]
	then
		[[ -e "$b" && -e "$m" ]] || return

		# Read current and maximum brightness.
		n="`< "$p/max_brightness"`" || return
		v="`< "$p/brightness"`" || return
		[[ "$v" == "$n" && "$h" != "0" && "$h" != "1" ]] && i="$((n-v))" || i="$n"
		[[ "$h" == "0" ]] && h="+" && i="0"
		[[ "$h" == "1" ]] && h="+" && i="$n"

		# Toggle brightness.
		[[ "$v" == "$n" && "$h" == "-" ]] && h="+"
		[[ "$h" == "+" ]] && echo "$i" | tee "$b"
	fi

	[[ "$h" != "+" ]] && p "${a[@]}"
	[[ "$h" == "-" ]] && echo "$i" | tee "$b"
}

# Check user ID.
u="`id -u`" || exit
[[ "$u" == "0" ]] || exec sudo "$0" "$DISPLAY" "$DBUS_SESSION_BUS_ADDRESS" "$@" || exit

# Get environment from command line.
a=( "${@:1:2}" )
f="$3"
h="${3+"_"}"

# Convert argument to flag according to present/empty value.
[[ "$f" ]] && h="+"
[[ "$f" == "0" || "$f" == "1" ]] && h="$f"
[[ "$h" ]] || h="-"

# Check dependencies.
s "${a[@]}" || exit

# Acquire lock to avoid multiple instances.
mkdir "$r" || exit

# Configure display backlight.
k "$h" "${a[@]}"

# Release lock.
rmdir "$r"
