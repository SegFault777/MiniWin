/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_VERSION_H
#define MW_VERSION_H

/* version.h -- the ONE place the version number lives. Bump it here and the desktop hint, `ver` and `fetch` follow (the hint
 * text said "1.1-ALPHA-1" for six releases because it was typed in three places). The same number still has to be changed by
 * hand in README.md and docs/project/CHANGELOG.md: tools/test/check_version.sh compares them. */
#define MW_VERSION      "1.1-alpha-10"
#define MW_VERSION_UP   "1.1-ALPHA-10"
#define MW_SLOGAN       "Mini OS, Many Things."

#endif
