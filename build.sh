#!/bin/bash
set -e

ARCH=arm64
LLVM=1
JOBS=$(nproc)
OUT=out
CLANG_DIR=~/toolchains/clang-x86/clang-r547379/bin

export PATH=${CLANG_DIR}:${PATH}

GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[BUILD]${NC} $1"; }
err() { echo -e "\033[0m[ERROR] $1"; exit 1; }

set_version() {
    [ -z "$VERSION" ] && return
    sed -i "s/^EXTRAVERSION.*/EXTRAVERSION = -${VERSION}/" Makefile
    log "Version: ${VERSION}"
}

while getopts "v:j:c" opt; do
    case $opt in
        v) VERSION="$OPTARG" ;;
        j) JOBS="$OPTARG" ;;
        c) make O=$OUT ARCH=$ARCH LLVM=1 mrproper; exit 0 ;;
        *) err "Invalid option" ;;
    esac
done

set_version

log "Config..."
make O=$OUT ARCH=$ARCH LLVM=1 gki_defconfig

log "Building -j${JOBS}..."
make O=$OUT ARCH=$ARCH LLVM=1 -j${JOBS} Image

log "Done! ${OUT}/arch/arm64/boot/Image"
