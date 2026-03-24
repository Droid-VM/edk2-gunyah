#!/bin/bash
VERSION=0.1
cd "$(dirname "$(realpath "$0")")/.."
if ! [ -d .git ]; then
	echo "$VERSION"
	exit 0
fi
ver="$(git describe --long --tags 2>/dev/null | sed 's/^[vV]//;s/\([^-]*-g\)/r\1/;s/-/./g')"
if ! [ -z "$ver" ]; then
	echo "$ver"
	exit 0
fi
cnt="$(git rev-list --count HEAD 2>/dev/null||true)"
sha="$(git rev-parse --short HEAD 2>/dev/null||true)"
if [ -n "$cnt" ] && [ -n "$sha" ]; then
	echo "${VERSION}.r${cnt}.${sha}"
	exit 0
fi
echo "${VERSION}"
