#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# The version in kernel/version.h must be the one README.md calls current and docs/project/CHANGELOG.md lists first.
set -euo pipefail
cd "$(dirname "$0")/../.."
V=$(sed -n 's/^#define MW_VERSION  *"\(.*\)"/\1/p' kernel/version.h)
UP=$(sed -n 's/^#define MW_VERSION_UP  *"\(.*\)"/\1/p' kernel/version.h)
[ "$(echo "$V" | tr a-z A-Z)" = "$UP" ] || { echo "version.h: MW_VERSION_UP ($UP) is not the upper-case form of MW_VERSION ($V)"; exit 1; }
grep -q "\*\*Current version: $V\*\*" README.md || { echo "README.md does not say 'Current version: $V'"; exit 1; }
FIRST=$(grep -m1 '^## MiniWin ' docs/project/CHANGELOG.md | sed 's/^## MiniWin \([^ ]*\).*/\1/')
[ "$FIRST" = "$V" ] || { echo "docs/project/CHANGELOG.md's newest entry is $FIRST, not $V"; exit 1; }
echo "version $V: kernel/version.h, README.md and docs/project/CHANGELOG.md agree"
