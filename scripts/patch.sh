#!/bin/bash
set -e

function patch_submodules() {
	grep '^\[submodule ".*"\]$' < .gitmodules |\
	sed -E 's,^\[submodule "(.*)"\]$,\1,g' |\
	while read -r submodule; do
		submod_id="${submodule//\//-}"
		for patch in "patches/$submod_id"/*.patch; do
			if ! [ -f "$patch" ]; then
				continue
			fi
			if patch -d "$submodule" -sfRp1 --dry-run < "$patch" &>/dev/null; then
				continue
			fi
			echo "Apply $patch to $submodule"
			patch -d "$submodule" -tNp1 < "$patch"
		done
	done
}

function check_tool() {
	if ! which "$1" &>/dev/null; then
		echo "$1 not found" >&2
		exit 1
	fi
}

function main() {
	check_tool git
	check_tool awk
	check_tool sed
	check_tool grep
	check_tool patch
	if ! [ -d .git ]; then
		exit 0
	fi
	patch_submodules
}

cd "$(dirname "$0")/../"
main
