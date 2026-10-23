#!/bin/sh
# ci-smoke-job.sh — what the release pipeline runs for the smoke stage.
#
# The pipeline's script: line used to hold this sequence in one quoted `sh -c`,
# which made the order impossible to read or check. The order is the point:
# tzdata has to be configured before any apt transaction that could need it, and
# gcc and binutils have to be installed before the smoke script asks for a
# compiler and for strings.
#
# Both tool packages are the *job's* need, not the packages' — the smoke script
# degrades those two checks to "did not run" notes when they are missing, which
# is honest but is not coverage. So the job provisions them and lets a failed
# apt stop the stage rather than quietly shrink what gets checked.
#
# The smoke script prepares tzdata for its own transaction too, so CI pays for
# that step twice. It is idempotent, and it is what keeps each script usable on
# a host of its own instead of only through this wrapper.
#
# Run from the checkout, in the same workspace as scripts/ci-build.sh:
#
#   CEFNEST_PACKAGE_RELEASE=8 ./scripts/ci-smoke-job.sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

SUDO=""
if [ "$(id -u)" != 0 ]; then SUDO=sudo; fi

export DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC

$SUDO apt-get update -qq
$SUDO ./scripts/ci-prepare-tzdata.sh
$SUDO apt-get install -y --no-install-recommends gcc binutils
exec ./scripts/ci-smoke-packages.sh
