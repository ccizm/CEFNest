#!/bin/sh
# ci-prepare-tzdata.sh — get tzdata configured before a transaction needs it.
#
# The CNB runners bind-mount /etc/localtime from the host. tzdata's postinst
# renames /etc/localtime.dpkg-new over it, and rename(2) on a mount point is
# EBUSY: the postinst exits non-zero, apt takes the whole transaction down with
# it, and every package after tzdata is left unpacked. On 2026-10-23 that shape
# was the -8 smoke stage's only failure — tzdata at the head, then python3.12,
# python3, libpython3.12-stdlib and the GTK -dev chain behind it, with our own
# two packages simply never reaching configure.
#
# So tzdata is dealt with on its own, in one of two ways: unmount the file and
# install normally, or — when unmounting is not permitted — unpack it and skip
# only that rename. /etc/timezone is still written, and callers export TZ so
# nothing later depends on /etc/localtime.
#
# Run it after apt-get update (the download branch needs the package list),
# before the install that would otherwise drag tzdata in:
#
#   apt-get update
#   ./scripts/ci-prepare-tzdata.sh
#
# Both the build stage and the package smoke stage call this. They used to
# carry the workaround separately, and the smoke copy had only the umount half —
# which is exactly the half a container that forbids umount does not have.

set -eu

SUDO=""
if [ "$(id -u)" != 0 ]; then SUDO=sudo; fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "tzdata-prep: no apt-get (this is a Debian/Ubuntu-only step)" >&2
  exit 1
fi

# tzdata's postinst asks debconf for a zone even in a bare Ubuntu image.
# Preseed it as well as setting the frontend: a postinst that cannot answer
# aborts the whole apt transaction, and every package after it is left unpacked.
if command -v debconf-set-selections >/dev/null 2>&1; then
  printf 'tzdata tzdata/Areas select Etc\n' | $SUDO debconf-set-selections
  printf 'tzdata tzdata/Zones/Etc select UTC\n' | $SUDO debconf-set-selections
fi

if awk '$2 == "/etc/localtime" { found = 1 } END { exit !found }' \
     /proc/mounts 2>/dev/null; then
  $SUDO umount /etc/localtime 2>/dev/null || true
fi

if awk '$2 == "/etc/localtime" { found = 1 } END { exit !found }' \
     /proc/mounts 2>/dev/null; then
  TZ_TMP=$(mktemp -d)
  (cd "$TZ_TMP" && apt-get download tzdata)
  set -- "$TZ_TMP"/tzdata_*.deb
  $SUDO dpkg --unpack "$1"
  TZ_POSTINST=/var/lib/dpkg/info/tzdata.postinst
  if ! $SUDO grep -q 'if test "$(get_timezone)" != "$TIMEZONE"; then' \
       "$TZ_POSTINST"; then
    echo "tzdata-prep: unexpected tzdata postinst; refusing to guess" >&2
    exit 1
  fi
  $SUDO sed -i \
    's|if test "$(get_timezone)" != "$TIMEZONE"; then|if false; then|' \
    "$TZ_POSTINST"
  $SUDO dpkg --configure tzdata
  rm -rf "$TZ_TMP"
  echo "tzdata-prep: /etc/localtime is still a mount point; the rename was skipped"
else
  $SUDO apt-get install -y --no-install-recommends tzdata
  echo "tzdata-prep: tzdata installed normally"
fi

# Repair anything an earlier failed run left unpacked, now that tzdata can be
# configured without touching the mount.
$SUDO dpkg --configure -a
$SUDO apt-get -f install -y
