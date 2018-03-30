#!/usr/bin/env bash

killall bamfdaemon gnome-software goa-daemon update-notifier deja-dup-monitor goa-identity-service

a=( `pgrep evolution` `pgrep tracker` `pgrep zeitgeist` )
[[ "${#a[@]}" == "0" ]] || kill "${a[@]}"

sudo killall aptd fwupd snapd udisksd ubuntu-geoip-provider geoclue-master

[[ "${1+"_"}" ]] && killall onboard
[[ "$1" ]] && killall nautilus
