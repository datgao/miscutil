#!/usr/bin/env bash
pv -ptrabfeW "$@" 2> >(tr '\r' '\n' >&2)
