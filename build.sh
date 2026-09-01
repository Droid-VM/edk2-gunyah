#!/bin/bash
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
VERSION="$(bash -$- scripts/version.sh)"
bash -$- scripts/patch.sh
set +x
SAVED_ARGS=("$@")
set --
source "${PWD}/edk2/edksetup.sh"
set -- "${SAVED_ARGS[@]}"
set -x
mkdir -pv "${WORKSPACE}"
# Serial on purpose. BaseTools' VfrCompile makefile declares EfiVfrParser.cpp,
# VfrSyntax.cpp, VfrParser.dlg and VfrTokens.h as four independent targets, but all four
# are produced by one antlr run -- so a parallel make starts an antlr per target and they
# overwrite each other's output. What comes out is a source file truncated mid-literal,
# and the compiler reports it as a C++ syntax error ("unable to find numeric literal
# operator") in generated code nobody wrote, which is about as far from the cause as an
# error message gets. It is also flaky: the last antlr to finish leaves a correct file, so
# rebuilding often "fixes" it. BaseTools is small; the whole of it serial costs less than
# one wrong diagnosis of this.
make -C "${EDK_TOOLS_PATH}" -j1
build \
	-t GCC \
	-a AARCH64 \
	-b "${EDK2_TARGET}" \
	-D DISABLE_NEW_DEPRECATED_INTERFACES=TRUE \
	-D FIRMWARE_VER="${VERSION}" \
	-p GunyahPkg/GunyahKernel.dsc \
	"$@"
rm -fv edk2-gunyah.fd edk2-gunyah.vars.fd
cp -v "${WORKSPACE}/Build/GunyahKernel-AARCH64/${EDK2_TARGET}_GCC/FV/GUNYAH_EFI.fd" edk2-gunyah.fd
cp -v "${WORKSPACE}/Build/GunyahKernel-AARCH64/${EDK2_TARGET}_GCC/FV/GUNYAH_VARS.fd" edk2-gunyah.vars.fd
ls -lh edk2-gunyah.fd edk2-gunyah.vars.fd
exit 0
