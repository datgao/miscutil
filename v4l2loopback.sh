#!/usr/bin/env bash

fmtv4l2() {
	local r s t
	t="${1:-"/dev/video1"}"
	s="${2:-"1280x960"}"
	r="${3:-"15"}"
	ffmpeg -analyzeduration 0 -f rawvideo -s "$s" -r "$r" -pix_fmt rgb24 -i /dev/zero -frames 1 -vcodec rawvideo -pix_fmt yuv420p -f v4l2 "$t"
}

ffv4l2() {
	local n p r s t v
	v="${1:-"/dev/video0"}"
	t="${2:-"/dev/video1"}"
	r="${3:-"15"}"
	s="${4:-"1280x960"}"
	n="$#"
	[[ "$n" == "0" ]] || p="${@:n}"
	ffmpeg -analyzeduration 0 -f video4linux2 -video_size "$s" -input_format mjpeg -framerate "$r" -i "$v" -vcodec rawvideo -pix_fmt yuv420p -vf transpose="${p:-"clock"}" -f v4l2 "$t"
}

x11v4l2() {
	ffmpeg -analyzeduration 0 -f x11grab -video_size 800x1280 -framerate 1 -i :0.0 -vcodec rawvideo -pix_fmt yuv420p -f v4l2 /dev/video1
}

lkmod() {
	local u
	local -a c
	[[ -e "$1" ]] && return
	c=( modprobe v4l2loopback devices=8 exclusive_caps=0,0,0,0,0,0,0,0 )
	u="`id -u`" || return
	[[ "$u" == "0" ]] || c=( sudo "${c[@]}" )
	"${c[@]}"
}

main() {
	local b c e i k p r s t v z
	local -a a
	[[ "$1" ]] || e="${1+"_"}"
	p="$1"
	b="/dev/video"
	c="${b}0"
	t="${b}1"
	a=( "960x1280" "1280x960" "1280x800" )
	r="15"

	lkmod "$t" || return

	# If first argument is empty string then configure video loopback devices with sticky settings.
	if [[ "$e" ]]
	then
		i="0"
		for v in "$b"*
		do
			[[ "$v" == "$c" ]] && continue
			for k in 0 1
			do
				v4l2-ctl --device="$v" --set-ctrl=keep_format="$k" || return
			done
			s="${a[i++]}"
			[[ "$s" ]] || break
			fmtv4l2 "$v" "$s" "$r" || return
		done
		return
	fi

	s="${a[0]}"
	w="${s%x*}"
	h="${s#*x}"
	z="${h}x$w"

	ffv4l2 "$c" "$t" "$r" "$z" "$p"
}

main "$@"
