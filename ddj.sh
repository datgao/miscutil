#!/usr/bin/env bash
c=( "$@" )
i="0"
h="127.0.0.1"
p=""
q=""
r=""

for(( i = 0; i < 3; i++ ))
do
	t="${c[i]}"
	[[ "$t" ]] || break
	v="${t%%[0-9]*}"
	s="${t:2}"

	if [[ "$v" == "" ]]
	then
		[[ "$p" == "" ]] || exit
		p="$t"
	elif [[ "$v" == "+>" ]]
	then
		[[ "$s" && "$q" == "" ]] || exit
		q="$s"
	elif [[ "$v" == "+<" ]]
	then
		[[ "$s" && "$r" == "" ]] || exit
		r="$s"
	else
		break
	fi
	unset c[i]
done

ssh "${c[@]}" -o ProxyCommand="bash -c `printf '%q' "( \
pv -trabfWB 4096 ${q:+-L "$q"} 2> >(awk -vRS=\$'\r' '{ printf("'"'"\r\033[A\033[A\033[31;1mclient:%%s\033[m\n\n"'"'", \\\$0) ; fflush("'""'") }' >&2) \
| nc %h %p \
| pv -trabfWB 4096 ${r:+-L "$r"} 2> >(awk -vRS=\$'\r' '{ printf("'"'"\r\033[A\033[32;1mserver:%%s\033[m\n"'"'", \\\$0) ; fflush("'""'") }' >&2) \
) 2> >(nc $h $p &>/dev/null )"`"

# while true ; do pv -q ${q:+-L "$q"} & w="'"$!"'" ; sleep 5 || e="'"$?"'" ; kill "'"$w"'" ; [[ "'"$'"{e:-"0"}"'"'" == "0" ]] || exit ; done \
# | while true ; do pv -q ${r:+-L "$r"} & w="'"$!"'" ; sleep 5 || e="'"$?"'" ; kill "'"$w"'" ; [[ "'"$'"{e:-"0"}"'"'" == "0" ]] || exit ; done \
#
