#!/usr/bin/env bash
set -euo pipefail

NAME="suse-monad"
VERSION="1.0.0"
TOPDIR="${HOME}/rpmbuild"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SPEC="${SCRIPT_DIR}/${NAME}.spec"
SOURCE_TARBALL="${SCRIPT_DIR}/${NAME}-${VERSION}.tar.gz"
CHANGES="${SCRIPT_DIR}/${NAME}.changes"
INSTALL_DEPS=1
BUILD_SRPM=1

usage() {
  cat <<USAGE
Usage: $0 [options]

Options:
  --topdir PATH      rpmbuild topdir (default: ~/rpmbuild)
  --no-deps          do not install build dependencies with zypper
  --rpm-only         build binary RPM only (rpmbuild -bb)
  --help             show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --topdir)
      TOPDIR="$2"
      shift 2
      ;;
    --no-deps)
      INSTALL_DEPS=0
      shift
      ;;
    --rpm-only)
      BUILD_SRPM=0
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

if [[ ! -f "$SPEC" ]]; then
  echo "Missing spec file: $SPEC" >&2
  exit 1
fi

if [[ ! -f "$SOURCE_TARBALL" ]]; then
  echo "Missing source tarball: $SOURCE_TARBALL" >&2
  exit 1
fi

if [[ $EUID -ne 0 && $INSTALL_DEPS -eq 1 ]]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO=sudo
  else
    echo "Run as root or install dependencies manually (rpm-build gcc make)." >&2
    exit 1
  fi
else
  SUDO=
fi

if [[ $INSTALL_DEPS -eq 1 ]]; then
  ${SUDO} zypper --non-interactive install --no-recommends rpm-build gcc make
fi

mkdir -p "$TOPDIR"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
install -m 0644 "$SOURCE_TARBALL" "$TOPDIR/SOURCES/"
install -m 0644 "$SPEC" "$TOPDIR/SPECS/"
if [[ -f "$CHANGES" ]]; then
  install -m 0644 "$CHANGES" "$TOPDIR/SOURCES/"
fi

if [[ $BUILD_SRPM -eq 1 ]]; then
  rpmbuild --define "_topdir $TOPDIR" -ba "$TOPDIR/SPECS/${NAME}.spec"
else
  rpmbuild --define "_topdir $TOPDIR" -bb "$TOPDIR/SPECS/${NAME}.spec"
fi

echo
echo "Build finished. Artifacts:"
find "$TOPDIR/RPMS" "$TOPDIR/SRPMS" -type f \( -name '*.rpm' -o -name '*.src.rpm' \) -print | sort
