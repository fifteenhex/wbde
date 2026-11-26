#!/bin/sh
# fetch-smol.sh NAME...: check each smol* library out beside wbde, at
# $<NAME>_URL and $<NAME>_REF (a commit or a branch), as meson.options
# expects (../smolinput and so on).
set -e
for name in "$@"; do
	var=$(echo "$name" | tr a-z A-Z)
	eval url=\$${var}_URL
	eval ref=\$${var}_REF
	if [ -z "$url" ] || [ -z "$ref" ]; then
		echo "${var}_URL and ${var}_REF must be set (repository variables)" >&2
		exit 1
	fi
	git init -q "$name"
	git -C "$name" fetch -q --depth 1 "$url" "$ref"
	git -C "$name" checkout -q FETCH_HEAD
	echo "$name: $(git -C "$name" rev-parse HEAD)"
done
