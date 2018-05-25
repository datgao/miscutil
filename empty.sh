#!/usr/bin/env bash
sigfn() { printf '\x1b[?25h' ; }
trap sigfn SIGTERM SIGINT SIGQUIT SIGHUP SIGPIPE
reset ; printf '\x1b[?25l' ; read ; sigfn
