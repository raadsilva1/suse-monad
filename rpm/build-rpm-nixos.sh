#!/usr/bin/env bash
set -euo pipefail

NAME="suse-monad"
VERSION="1.0.0"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TOPDIR="${TOPDIR:-${HOME}/rpmbuild}"
SPEC="${SCRIPT_DIR}/${NAME}.spec"
SOURCE_TARBALL="${SCRIPT_DIR}/${NAME}-${VERSION}.tar.gz"
BUILD_SRPM=1
VERBOSE=0

usage() {
  cat <<USAGE
Usage: $0 [options]

Options:
  --topdir PATH      rpmbuild topdir (default: ~/rpmbuild)
  --rpm-only         build binary RPM only (rpmbuild -bb)
  --verbose          show rpmbuild command
  --help             show this help

Examples:
  nix develop -c ./build-rpm-nixos.sh
  nix run .#build-rpm -- --rpm-only
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --topdir)
      TOPDIR="$2"
      shift 2
      ;;
    --rpm-only)
      BUILD_SRPM=0
      shift
      ;;
    --verbose)
      VERBOSE=1
      shift
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

for cmd in rpmbuild install find tar gzip gcc make; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Missing required command in PATH: $cmd" >&2
    echo "On NixOS, enter the shell with: nix develop -c ./build-rpm-nixos.sh" >&2
    exit 1
  fi
done

if [[ ! -f "$SPEC" ]]; then
  echo "Missing spec file: $SPEC" >&2
  exit 1
fi
if [[ ! -f "$SOURCE_TARBALL" ]]; then
  echo "Missing source tarball: $SOURCE_TARBALL" >&2
  exit 1
fi

mkdir -p "$TOPDIR"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
install -m 0644 "$SOURCE_TARBALL" "$TOPDIR/SOURCES/"
install -m 0644 "$SPEC" "$TOPDIR/SPECS/"
if [[ -f "${SCRIPT_DIR}/${NAME}.changes" ]]; then
  install -m 0644 "${SCRIPT_DIR}/${NAME}.changes" "$TOPDIR/SOURCES/"
fi

RPMBUILD_CMD=(rpmbuild --define "_topdir $TOPDIR")
if [[ $BUILD_SRPM -eq 1 ]]; then
  RPMBUILD_CMD+=( -ba "$TOPDIR/SPECS/${NAME}.spec" )
else
  RPMBUILD_CMD+=( -bb "$TOPDIR/SPECS/${NAME}.spec" )
fi

if [[ $VERBOSE -eq 1 ]]; then
  printf 'Running:'
  for x in "${RPMBUILD_CMD[@]}"; do
    printf ' %q' "$x"
  done
  printf '\n'
fi

"${RPMBUILD_CMD[@]}"

echo
echo "Build finished. Artifacts:"
find "$TOPDIR/RPMS" "$TOPDIR/SRPMS" -type f \( -name '*.rpm' -o -name '*.src.rpm' \) -print | sort
