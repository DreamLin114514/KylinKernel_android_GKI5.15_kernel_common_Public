#!/bin/bash
set -e

ARCH=arm64
LLVM=1
JOBS=$(nproc)
OUT=out
CLANG_DIR=~/toolchains/clang-x86/clang-r547379/bin
MODULES_OUT=modules_out

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

# Auto kernel version from date + build type (unless overridden with -v)
if [ -z "$VERSION" ]; then
    DATE=$(date +%y%m%d)
    log "Build date: ${DATE}"
    while true; do
        read -rp "Build type? [R]elease / [T]est (default: Test): " choice
        case "${choice,,}" in
            r|release) TYPE="Release" ;;
            t|test|"") TYPE="Test" ;;
            *) log "Invalid input, please enter R or T"; continue ;;
        esac
        break
    done
    VERSION="KylinKernel-${DATE}-${TYPE}"
    log "Auto version: ${VERSION}"
fi

set_version

log "Config..."
make O=$OUT ARCH=$ARCH LLVM=1 gki_defconfig

log "Building kernel -j${JOBS}..."
make O=$OUT ARCH=$ARCH LLVM=1 -j${JOBS} Image modules

log "Collecting modules..."
rm -rf ${MODULES_OUT}
mkdir -p ${MODULES_OUT}
find ${OUT} -name "*.ko" -exec cp {} ${MODULES_OUT}/ \;
cp drivers/moon/*.ko ${MODULES_OUT}/ 2>/dev/null || true

log "Done!"
log "  Image:   ${OUT}/arch/arm64/boot/Image"
log "  Modules: ${MODULES_OUT}/"
ls -lh ${MODULES_OUT}/
