#!/usr/bin/env bash
# build_grub.sh — build a local GRUB 2.14 toolchain for the ISO build.
#
# The Makefile prefers build/grub-2.14/install/{bin,lib/grub} over the
# distro's grub-pc-bin/grub-efi-amd64-bin (Ubuntu noble still ships 2.12).
# Two platform builds are installed into one prefix:
#
#   build/grub-2.14/build-i386-pc     BIOS core (grub-mkrescue i386-pc mods)
#   build/grub-2.14/build-x86_64-efi  EFI modules (grub-mkstandalone image)
#   build/grub-2.14/install           shared bin/ + lib/grub/<platform>/
#
# Requires: gcc multilib (-m32), bison, flex, python3, make, xz.
# The tarball is cached in $UAOS_CACHE_DIR (default /tmp/uaos-dl-cache).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GRUB_VER=2.14
WORKDIR="${REPO_ROOT}/build/grub-${GRUB_VER}"
SRCDIR="${WORKDIR}/grub-${GRUB_VER}"
PREFIX="${WORKDIR}/install"
CACHE="${UAOS_CACHE_DIR:-/tmp/uaos-dl-cache}"
TARBALL="grub-${GRUB_VER}.tar.xz"
URL="https://ftp.gnu.org/gnu/grub/${TARBALL}"

PF2_OK=1
if [ ! -f "${PREFIX}/share/grub/unicode.pf2" ] && \
   [ -f /usr/share/grub/unicode.pf2 ]; then
    PF2_OK=0
fi

if [ -x "${PREFIX}/bin/grub-mkrescue" ] && \
   [ -d "${PREFIX}/lib/grub/i386-pc" ] && \
   [ -d "${PREFIX}/lib/grub/x86_64-efi" ] && \
   [ "${PF2_OK}" = "1" ]; then
    echo "grub ${GRUB_VER} already installed at ${PREFIX}"
    exit 0
fi

mkdir -p "${WORKDIR}" "${CACHE}"

if [ ! -d "${SRCDIR}" ]; then
    if [ ! -f "${CACHE}/${TARBALL}" ]; then
        echo "FETCH   ${TARBALL}"
        wget -q --timeout=60 --tries=2 -O "${CACHE}/${TARBALL}" "${URL}"
    else
        echo "CACHE   ${TARBALL}"
    fi
    echo "EXTRACT ${TARBALL}"
    tar -xf "${CACHE}/${TARBALL}" -C "${WORKDIR}"
fi

build_platform() {
    local name="$1"; shift
    local bdir="${WORKDIR}/build-${name}"
    mkdir -p "${bdir}"
    if [ ! -f "${bdir}/Makefile" ]; then
        echo "CONF    ${name}"
        (cd "${bdir}" && "${SRCDIR}/configure" \
            --prefix="${PREFIX}" --disable-werror --quiet "$@")
    fi
    echo "MAKE    ${name}"
    make -C "${bdir}" -j"$(nproc)" -s
    echo "INSTALL ${name}"
    make -C "${bdir}" install -s
}

# x86_64-efi first: its bin/ tools land in the shared prefix, then i386-pc
# adds the BIOS module tree (lib/grub/i386-pc) next to x86_64-efi's.
build_platform x86_64-efi --target=x86_64 --with-platform=efi
build_platform i386-pc   --target=i386   --with-platform=pc \
    TARGET_CC="gcc -m32"

# unicode.pf2: grub-mkrescue needs a console font.  When freetype2 was
# absent grub-mkfont wasn't built, so stage the distro's copy — the pf2
# format is stable across GRUB releases (same unifont source).
if [ ! -f "${PREFIX}/share/grub/unicode.pf2" ]; then
    if [ -f /usr/share/grub/unicode.pf2 ]; then
        cp /usr/share/grub/unicode.pf2 "${PREFIX}/share/grub/unicode.pf2"
        echo "STAGE   unicode.pf2 (from system grub-common)"
    else
        echo "WARN    no unicode.pf2 — gfxterm may not render"
    fi
fi

"${PREFIX}/bin/grub-mkrescue" --version | head -1
echo "grub ${GRUB_VER} installed at ${PREFIX}"
