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

filezero() {
	local b e m p s
	e="${1:-"/dev/null"}" && s="${2:-"0"}" && b="${3:-"16777216"}" && m="${4:-"256"}"
	if pv -qSs 0 < /dev/null &> /dev/null
	then
		time pv -ptrabfeWB "$b" -Ss "$s" -N zero -i 10 /dev/zero 2> >(tr '\r' '\n' >&2) > "$e"
	else
		time while (( b > 0 ))
		do
			p="$((s/b))"
			s="$((s%b))"
			dd if=/dev/zero bs="$b" count="$p" status=none 2> /dev/null || return
			b="$((b/m))"
		done > "$e"
		# | time pv -ptrabfeWB "$b" -s "$s" -N zero -i 10 2> >(tr '\r' '\n' >&2) > "$e"
	fi
}

fat32magic() {
	local -a a c
	local b f g m p q r s t u v w x
	m="$1" && f="$2" && g="$3" && c=( "${@:4}" )
	q=1
	[[ "${#c[@]}" == 1 && "${#c[0]}" == 0 ]] && c=( strace -fx ) && q=""
	[[ "${c[0]:0:1}" == "-" ]] && a=( "${c[@]}" ) && c=( ) && v=1
	[[ "${#c[@]}" == 1 && "${#c[0]}" == 1 ]] && c=( gdb -ex run -ex bt -ex q -ex y --args )
	if [[ "${#a[@]}" == "1" && "${a[0]:0:1}" == "-" ]]
	then
		[[ "${a[0]:1}" == "-" ]] && a=( ) && q=""
		[[ "${a[0]:1}" == "_" ]] && a=( ) && v=1
		[[ "${a[0]:1}" == "+" ]] && a=( ) && v=1 && q=""
		[[ "${a[0]:1}" == "#" ]] && a=( "-b" ) && q=""
		[[ "${a[0]:1}" == "@" ]] && a=( "-b" ) && v=1
		[[ "${a[0]:1}" == "=" ]] && a=( "-b" ) && v=1 && q=""
	fi
	echo "'$m'/"
	blockdev --getsz "$g"
	blockdev --getsz "$f"
	echo
	u="fat32g"
	t="."
	x="$t/$u"
	if [[ ! -e "$x" ]] ; then t="${0%/*}" && [[ "$0" != "$t" ]] && x="$t/$u" && [[ -e "$x" ]] || return ; fi
	# do not make sparse e.g. bad: truncate -s 12345 "$t"
	[[ "$v" ]] && e="$m/file.ext" && filezero "$e" 4294967295
	t="$m/fat32.c"
	w="$m/asm.c"
	cp -nv fat32.c "$t"
	cp -nv asm.c "$w"
	time sync --file-system "$m/."
	time echo -n 3 | tee /proc/sys/vm/drop_caches
	echo
	time time "${c[@]}" "$x" -m "$m" -d "$g" -p "$f" ${q:+"-g"} "${a[@]}" < <(printf '%s\n' ${e:+"$e"} "$t" "$w")
	printf '%s\n\n' "'$?'"
	dosfsck -rfv "$g" < <(printf '%s\n' "1" "y")
	printf '%s\n\n' "'$?'"
}

fnmain() {
	local -a
	local d e f g m p r s t w z
	a=( "$@" )

	for d in /dev/zram*
	do
		[[ -e "$d" ]] || continue
		iskdevbusy "$d" && z="$d" && break
	done

	s="$((16 * 1024 * 1048576))"
	cfgzram "$z" "$s" || return
	# which blkdiscard &>/dev/null && ! blkdiscard -v "$z" && r="1"

	[[ "$r" ]] || ! g="`losetup -f`" || cfgloop "$g" "$z" || ! r="$?" || g=""
	p="$((32 * 1048576))"
	[[ "$r" ]] || ! f="`losetup -f`" || cfgloop "$f" "$g" "$p" || ! r="$?" || f=""

	[[ "$r" ]] || mke4fspart "$f" "$g" "$p" "$s" || r="$?"
	m="/mnt"
	[[ "$r" ]] || mountfs "$f" "$m" || ! r="$?" || m=""

	[[ "$r" ]] || fat32magic "$m" "$f" "$g" "${a[@]}" || r="$?"

	if [[ ! "$r" ]]
	then
		t="/srv"
		mount "$g" "$t"
		ls -akls "$t"
		echo
		w="$t/FSCK0000.REC" && [[ -e "$w" ]] || ! w="$t/FAT32.C" || e="1"
		cp -v "$w" /tmp/
		echo
		diff -sq fat32.c "$w"
		printf '%s\n\n' "'$?'"
		diff -sq asm.c "$t/asm.c"
		printf '%s\n\n' "'$?'"
		if [[ -f "$t/file.ext" ]]
		then
			time filezero /dev/stdout 4294967295 | time diff -sq - "$t/file.ext"
			printf '%s\n\n' "'$?'"
		fi
		if [[ ! "$e" ]] ; then tr -d '\000' < "$w" | diff -sq - fat32.c ; printf '%s\n\n' "'$?'" ; fi
		df -h | grep -E '[ \t]/(mnt|srv)$'
		echo
		umount "$t"
	fi

	# grep -HnF '' "/sys/block/${z##*/}"/{orig_data_size,compr_data_size,mem_used_total,zero_pages,comp_algorithm}

	[[ ! "$m" ]] || unmountfs "$m" || r="$?"
	[[ ! "$f" ]] || resetloop "$f" || r="$?"
	[[ ! "$g" ]] || resetloop "$g" || r="$?"
	[[ ! "$z" ]] || resetzram "$z" || r="$?"

	return "${r:-"0"}"
}

fnmain "$@"
