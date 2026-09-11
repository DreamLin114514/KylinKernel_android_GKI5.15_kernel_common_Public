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

# prompt helper: reads a single character, returns lowercased via tr (dash-safe)
ask_yesno() {
    # $1 = prompt text ; sets REPLY_VAL=1/0
    while true; do
        read -r -p "$1" ans
        ans=$(printf '%s' "$ans" | tr '[:upper:]' '[:lower:]')
        case "$ans" in
            y|yes) REPLY_VAL=1; return 0 ;;
            n|no|"") REPLY_VAL=0; return 0 ;;
            *) log "Invalid input, please enter Y or N"; continue ;;
        esac
    done
}

O3=
VERSION=
JOBS_SET=
while getopts "v:j:co" opt; do
    case $opt in
        v) VERSION="$OPTARG" ;;
        j) JOBS="$OPTARG" ;;
        c) make O=$OUT ARCH=$ARCH LLVM=1 mrproper; exit 0 ;;
        o) O3=1 ;;
        *) err "Invalid option" ;;
    esac
done
shift $((OPTIND - 1))

# Auto kernel version from date + build type (unless overridden with -v)
if [ -z "$VERSION" ]; then
    DATE=$(date +%y%m%d)
    log "Build date: ${DATE}"
    while true; do
        read -r -p "Build type? [R]elease / [T]est / [D]ev (default: Test): " choice
        choice=$(printf '%s' "$choice" | tr '[:upper:]' '[:lower:]')
        case "$choice" in
            r|release) TYPE="Release" ;;
            t|test|"") TYPE="Test" ;;
            d|dev) TYPE="Dev" ;;
            *) log "Invalid input, please enter R, T or D"; continue ;;
        esac
        break
    done
    VERSION="KylinKernel-${DATE}-${TYPE}"
    log "Auto version: ${VERSION}"
fi

set_version

# Ask for -O3 unless already forced via -o flag
if [ -z "$O3" ]; then
    ask_yesno "Enable -O3 optimization? [Y]es / [N]o (default: No): "
    O3=$REPLY_VAL
fi

log "Config..."
make O=$OUT ARCH=$ARCH LLVM=1 gki_defconfig

# Optional local fragment kconfig_override (merged on top of gki_defconfig)
# e.g. "# CONFIG_ZRAM is not set" to temporarily disable a feature.
# Delete/rename the file to build the full-featured kernel again.
if [ -f kconfig_override ]; then
    log "Merging kconfig_override:"
    sed 's/^/    /' kconfig_override
    scripts/kconfig/merge_config.sh -m -O $OUT \
        arch/arm64/configs/gki_defconfig kconfig_override >/dev/null
    make O=$OUT ARCH=$ARCH LLVM=1 olddefconfig
fi

log "Building kernel -j${JOBS}..."
CFLAGS_EXTRA=
if [ "$O3" = "1" ]; then
    log "Optimization: -O3 enabled (via KCFLAGS)"
    CFLAGS_EXTRA="KCFLAGS=-O3"
fi
# Use KCFLAGS (not KBUILD_CFLAGS) so the kernel's full built-in flag set is preserved.
# Passing KBUILD_CFLAGS on the command line would clobber all kbuild defaults.
make O=$OUT ARCH=$ARCH LLVM=1 $CFLAGS_EXTRA -j${JOBS} Image modules

log "Collecting modules..."
rm -rf ${MODULES_OUT}
mkdir -p ${MODULES_OUT}
find ${OUT} -name "*.ko" -exec cp {} ${MODULES_OUT}/ \;
cp drivers/moon/*.ko ${MODULES_OUT}/ 2>/dev/null || true

log "Done!"
log "  Image:   ${OUT}/arch/arm64/boot/Image"
log "  Modules: ${MODULES_OUT}/"
ls -lh ${MODULES_OUT}/