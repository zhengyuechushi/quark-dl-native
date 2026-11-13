#!/usr/bin/env python3
# encoding: utf-8
"""Turn assets/ca.pem into a C string constant.

Same idea as gen-asset-module.py: keep binary-ish data out of the source tree
and out of the manual steps. The bundle is a Mozilla-derived root list, so it is
stored as one string literal per PEM line instead of a byte array; 200 KB of
text becomes 200 KB of source rather than 1.4 MB of hex escapes.
"""

import argparse
import sys


HEADER = '''/* Generated from %(source)s by gen-ca-module.py. Do not edit. */
#include "ca.h"

static const char ca_pem[] =
'''

FOOTER = ''';

const char *
qdl_ca_bundle(void) {
  return ca_pem;
}
'''


def escape(line):
    out = []
    for ch in line:
        if ch == '\\\\':
            out.append('\\\\\\\\')
        elif ch == '"':
            out.append('\\\\"')
        elif ch == '\t':
            out.append('\\\\t')
        else:
            out.append(ch)
    return ''.join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('SOURCE')
    parser.add_argument('-o', '--output', default=None)
    args = parser.parse_args()

    with open(args.SOURCE, encoding='utf-8') as f:
        lines = f.read().splitlines()

    body = []
    count = 0
    for line in lines:
        body.append('  "%s\\n"\n' % escape(line))
        if line.startswith('-----BEGIN CERTIFICATE'):
            count += 1
    # A C string needs at least one element; guard the empty-bundle case.
    if not body:
        body.append('  ""\n')

    text = HEADER % {'source': args.SOURCE} + ''.join(body) + FOOTER
    if args.output:
        with open(args.output, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    sys.stderr.write('%s: %d certificates\n' % (args.SOURCE, count))


if __name__ == '__main__':
    main()
