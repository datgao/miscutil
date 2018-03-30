#!/usr/bin/env bash
time bash -c '\
rsmfn() { \
contstop.sh ; \
contstop.sh ; \
[[ -e /tmp/contstop.ff ]] || contstop.sh ; \
} ; \
sigfn() { rsmfn ; exit ; } ; \
trap sigfn SIGTERM SIGQUIT SIGINT SIGHUP ; \
rsmfn ; \
while true ; \
do \
for t in '"${1:-"0.07"}"' '"${2:-"0.07"}"' ; \
do \
sleep "$t" ; \
contstop.sh ; \
done ; \
done\
'

