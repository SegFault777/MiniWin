#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Documentation link check: every relative Markdown link in README.md and docs/**/*.md must point at a file that exists, and every
`docs/<path>.md` path mentioned anywhere in the tracked sources (kernel comments, tools, README) must exist too. Run from anywhere.
    tools/test/check_docs.py"""
import os, re, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
os.chdir(ROOT)
files = [f for f in subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard'], text=True).split('\n') if f]
bad = []
for f in files:
    if not os.path.exists(f): continue
    ext = f.rsplit('.', 1)[-1]
    if ext not in ('md', 'h', 'c', 'asm', 'py', 'sh'): continue
    try: s = open(f, encoding='utf-8').read()
    except Exception: continue
    if ext == 'md' and (f == 'README.md' or f.startswith('docs/')):
        for m in re.finditer(r'\]\(([^)\s]+)\)', s):
            t = m.group(1).split('#')[0]
            if not t or re.match(r'^[a-z]+:', t) or t == '../../releases': continue     # (a GitHub-relative link to the Releases page)
            if not os.path.exists(os.path.normpath(os.path.join(os.path.dirname(f), t))): bad.append('%s: broken link (%s)' % (f, m.group(1)))
    for m in re.finditer(r'docs/[A-Za-z0-9_./-]+\.(?:md|log|png)', s):
        if not os.path.exists(m.group(0)): bad.append('%s: mentions %s, which does not exist' % (f, m.group(0)))
if bad:
    print('\n'.join(sorted(set(bad)))); sys.exit(1)
print('docs: all links and docs/ paths resolve')
