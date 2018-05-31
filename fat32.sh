#!/usr/bin/env bash

# set -xET

u="`id -u`" || exit
[[ "$u" == "0" ]] || exec sudo "$0" "$@" || exit

iskdevbusy() {
	local f p r
	f="$1"
	[[ "$f" ]] || return
	r='^'"$f"'[ \t]'

	for p in "/proc/mounts" "/proc/swaps"
	do
		[[ -e "$p" ]] || return
		! grep -qE "$r" "$p" || return
	done
}

resetzram() {
	local b i r z
	z="$1"
	b="${z##*/}"

	for(( i = 0; i < 5; i++ ))
	do
		sleep "0.1"
		echo "1" > "/sys/block/$b/reset" && return || r="$?"
	done
	return "${r:-"0"}"
}

cfgzram() {
	local -a k v
	local b s z
	z="$1"
	s="$2"
	[[ "$s" ]] || s="$(( 512 * 1048576 ))"
	[[ -e "$z" ]] || return
	b="${z##*/}"

	k=( "reset"	"disksize"	)
	v=( "1"		"$s"		)

	for i in "${!k[@]}"
	do
		[[ "${v[i]+"_"}" ]] || break
		echo "${v[i]}" > "/sys/block/$b/${k[i]}" || return
	done
}

resetloop() {
	local f
	f="$1"
	losetup -d "$f"
}

cfgloop() {
	local f p s r z
	f="$1" && z="$2" && p="$3" && s="$4" && r="$5"
	losetup ${p:+-o "$p"} ${s:+--sizelimit "$s"} ${r:+-r} "$f" "$z"
}

mke4fspart() {
	local f g h j m n p s t u
	f="$1" && g="$2" && p="$3" && s="$4" && m="${5:-"4"}" && j="${5:+"_"}"
	h=255 && t=63 && n="$((s / 512))" && [[ "$j" ]] || u=",^has_journal"
	fdisk -c=dos -u=sectors -H "$h" -S "$t" -C "$(((n+h*t-1)/(h*t)))" "$g" < <(printf '%s\n' "n" "p" "1" "$((p / 512))" "" "w" "q") || echo "'$?'"
	fdisk -l "$g"
	echo
	mke2fs -b 4096 ${j:+-J size="$m"} -L ext4fs -m 1 -O sparse_super"$u" -t ext4 -v "$f" || return
	echo
}

mountfs() {
	mount "$1" "$2"
}

unmountfs() {
	umount "$1"
}

fat32magic() {
	local f g m p s t u r x
	m="$1" && f="$2" && g="$3" && p="$4" && s="$5" && r="$6"
	echo "'$m'/"
	blockdev --getsz "$g"
	blockdev --getsz "$f"
	echo
	u="fat32g"
	t="."
	x="$t/$u"
	if [[ ! -e "$x" ]] ; then t="${0%/*}" && [[ "$0" != "$t" ]] && x="$t/$u" && [[ -e "$x" ]] || return ; fi
	# t="$m/file.ext"
	# do not make sparse: truncate -s 12345 "$t"
	# dd if=/dev/zero bs=12345 count=1 of="$t"
	t="$m/fat32.c"
	cp -nv fat32.c "$t"
	echo
	# strace -fx ''
	"$x" -m "$m" -d "$g" -p "$f" -g < <(echo "$t")
	printf '\n%s\n\n' "'$?'"
	dosfsck -rfv "$g" < <(printf '%s\n' "1" "y")
	printf '\n%s\n\n' "'$?'"
}

fnmain() {
	local d e f g m p r s t w z

	for d in /dev/zram*
	do
		[[ -e "$d" ]] || continue
		iskdevbusy "$d" && z="$d" && break
	done

	s="$((16 * 1024 * 1048576))"
	cfgzram "$z" "$s" || return

	g="`losetup -f`" && cfgloop "$g" "$z" || ! r="$?" || g=""
	p="$((32 * 1048576))"
	[[ "$r" ]] || ! f="`losetup -f`" || cfgloop "$f" "$g" "$p" || ! r="$?" || f=""

	[[ "$r" ]] || mke4fspart "$f" "$g" "$p" "$s" || r="$?"
	m="/mnt"
	[[ "$r" ]] || mountfs "$f" "$m" || ! r="$?" || m=""

	[[ "$r" ]] || fat32magic "$m" "$f" "$g" || r="$?"

	if [[ ! "$r" ]]
	then
		t="/srv"
		mount "$g" "$t"
		ls -akls "$t"
		w="$t/FSCK0000.REC" && [[ -e "$w" ]] || ! w="$t/zOMGWTF.BBQ" || e="1"
		cp -v "$w" /tmp/
		diff -sq fat32.c "$w"
		printf '\n%s\n\n' "'$?'"
		if [[ ! "$e" ]] ; then tr -d '\000' < "$w" | diff -sq - fat32.c ; printf '\n%s\n\n' "'$?'" ; fi
		umount "$t"
	fi

	[[ ! "$m" ]] || unmountfs "$m" || r="$?"
	[[ ! "$f" ]] || resetloop "$f" || r="$?"
	[[ ! "$g" ]] || resetloop "$g" || r="$?"
	[[ ! "$z" ]] || resetzram "$z" || r="$?"

	return "${r:-"0"}"
}

fnmain "$@"
