#/usr/bin/env bash
a=( "Up" "Down" )
[[ "${1+"_"}" ]] && i="1" || i="0"

for n in 0 1
do
	xmodmap -e "keycode $((185+(n^i))) = ${a[n]}"
done

# xmodmap -e "keysym XF86ScrollUp = Up"
# xmodmap -e "keysym XF86ScrollDown = Down"
