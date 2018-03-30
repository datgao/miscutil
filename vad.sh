#!/usr/bin/env bash
mic() {
	local c m
	c="$1"
	m="$2"
	time ffmpeg -f pulse -acodec pcm_s16le -ar 48000 -ac "${c:-"2"}" -i "${m:-"default"}" -acodec copy -f wav -
}

teeflac() {
	local c f t
	f="$1"
	t="$2"
	c=( time ffmpeg -i - -f flac "$f" )
	if [[ "$t" ]]
	then
		tee >("${c[@]}")
	else
		"${c[@]}"
	fi
}

vad() {
	local d
	d="$1"
	time silan -pvv${d:+s "$d"} /dev/stdin
}

src() {
	local f r t
	f="$1"
	t="$2"
	r=( mic "${@:3}" )
	if [[ "$f" ]]
	then
		"${r[@]}" | teeflac "$f" "$t"
	else
		"${r[@]}"
	fi
}

sink() {
	local p t
	t="$1"
	p=( vad "${@:2}" )
	if [[ "$t" ]]
	then
		"${p[@]}" | tee "$t"
	else
		"${p[@]}"
	fi
}

chain() {
	local a b c d f t
	b="$1"
	d="$2"
	a=( "${@:3}" )

	f="$b.flac"
	t="$b.tsf"

	c=( src "${b:+"$f"}" "${d:+"_"}" "${a[@]}" )

	if [[ "$d" ]]
	then
		"${c[@]}" | sink "${b:+"$t"}" ${d:+"$d"}
	else
		"${c[@]}"
	fi
}

# base db ch mic
chain "$1${1:+"`date -u '+%s'`"}" "${@:2}"
