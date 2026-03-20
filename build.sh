#!/bin/bash
VERSION=0.1
set -ex
cd "$(dirname "$0")"
git submodule update --init --recursive --depth 1
EDK2_TARGET="${EDK2_TARGET:-RELEASE}"
export EDK_TOOLS_PATH="${PWD}/edk2/BaseTools"
export PACKAGES_PATH="${PWD}/edk2:${PWD}"
export WORKSPACE="${PWD}/build"
if ! gcc -dumpmachine 2>/dev/null | grep -q '^aarch64-'; then
    export GCC_AARCH64_PREFIX=aarch64-linux-gnu-
fi
if [ -d .git ]; then
	ver="$(git describe --long --tags 2>/dev/null | sed 's/^[vV]//;s/\([^-]*-g\)/r\1/;s/-/./g')"
	if [ -z "$ver" ]; then
		cnt="$(git rev-list --count HEAD 2>/dev/null||true)"
		sha="$(git rev-parse --short HEAD 2>/dev/null||true)"
		if [ -n "$cnt" ] && [ -n "$sha" ]; then
			VERSION="${VERSION}.r${cnt}.${sha}"
		fi
	else
		VERSION="$ver"
	fi
fi
set +x
source "${PWD}/edk2/edksetup.sh"
set -x
mkdir -pv "${WORKSPACE}"
make -C "${EDK_TOOLS_PATH}" -j"$(nproc)"
build \
	-t GCC \
	-a AARCH64 \
	-b "${EDK2_TARGET}" \
	-D DISABLE_NEW_DEPRECATED_INTERFACES=TRUE \
	-D FIRMWARE_VER="${VERSION}" \
	-p GunyahPkg/GunyahKernel.dsc
rm -fv edk2-gunyah.fd edk2-gunyah.vars.fd
cp -v "${WORKSPACE}/Build/GunyahKernel-AARCH64/${EDK2_TARGET}_GCC/FV/GUNYAH_EFI.fd" edk2-gunyah.fd
cp -v "${WORKSPACE}/Build/GunyahKernel-AARCH64/${EDK2_TARGET}_GCC/FV/GUNYAH_VARS.fd" edk2-gunyah.vars.fd
ls -lh edk2-gunyah.fd edk2-gunyah.vars.fd
exit 0
