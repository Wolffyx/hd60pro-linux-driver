#!/usr/bin/env bash
# Print, check or set the driver version.
#
# src/mz0380.h (MZ0380_VERSION) is the one source: the Makefile and
# nix/package.nix read it. dkms.conf has to carry a literal copy, because DKMS
# sources it as shell, so this keeps the two in step.
#
#   scripts/mz0380-version.sh          print it; fail if dkms.conf disagrees
#   scripts/mz0380-version.sh 0.3.0    set it in both files, then print it
#
# The release workflow runs the first form; releases are tagged v<version>.
set -euo pipefail
cd "$(dirname "$0")/.."

hdr=src/mz0380.h
read_hdr() { sed -n 's/^#define MZ0380_VERSION[[:space:]]*"\(.*\)"$/\1/p' "$hdr"; }

if [ $# -eq 1 ]; then
	[[ $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version must be X.Y.Z, got '$1'" >&2; exit 1; }
	sed -i "s/^\(#define MZ0380_VERSION[[:space:]]*\)\".*\"$/\1\"$1\"/" "$hdr"
	sed -i "s/^PACKAGE_VERSION=\".*\"$/PACKAGE_VERSION=\"$1\"/" dkms.conf
fi

ver=$(read_hdr)
dkms=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"$/\1/p' dkms.conf)
[ -n "$ver" ] || { echo "no MZ0380_VERSION in $hdr" >&2; exit 1; }
[ "$dkms" = "$ver" ] || { echo "dkms.conf says '$dkms' but $hdr says '$ver' - run: $0 $ver" >&2; exit 1; }
echo "$ver"
