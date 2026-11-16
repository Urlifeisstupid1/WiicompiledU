#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# gen_aurora_stubs.py - generates wiiu/aurora-wiiu/src/aurora_wiiu_stubs.cpp.
#
# Phase 2 of the Wii U port needs every aurora entry point the runtime links
# against (wiiu/aurora-wiiu/stub_symbols.txt) without any rendering. For each
# listed function that returns void, this finds its declaration in aurora's
# headers (wiiu/aurora-wiiu/include overrides first) and emits a definition that
# does nothing. Getters (GXGet*/AuroraGet*) zero their pointer out-parameters so
# callers never read garbage; no other function touches its pointer arguments,
# because several receive pointers into guest memory (GXLoadTexObj, ...).
# Functions that return a value, and names listed in MANUAL, are written by hand
# in aurora_wiiu.cpp. Phase 3 replaces these stubs with the GX2 backend.
#
# usage: gen_aurora_stubs.py <aurora include dir> <aurora-wiiu dir>
import os
import re
import sys

MANUAL = {
    'aurora_initialize', 'aurora_update', 'aurora_begin_frame', 'aurora_begin_frame_gx',
    'aurora_begin_frame_window', 'aurora_end_frame_window', 'aurora_get_display_mode',
    'GXInit', 'GXGetCPUFifo', 'GXGetTexBufferSize', 'PADInit', 'PADRead', 'PADRecalibrate',
    'PADReset', '__GXSetSUTexRegs', 'aurora::gx::fifo::submit_raw_draw', 'GXEnd',
}
TYPE_WORDS = {'int', 'char', 'long', 'short', 'unsigned', 'signed', 'float', 'double', 'bool', 'const',
              'volatile', 'struct', 'enum'}


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def load_headers(roots):
    headers = {}
    for root in roots:
        for dirpath, _, names in os.walk(root):
            for name in names:
                if name.endswith(('.h', '.hpp')):
                    path = os.path.join(dirpath, name)
                    headers.setdefault(os.path.relpath(path, root), path)  # first root wins
    return {rel: strip_comments(open(path, errors='replace').read()) for rel, path in headers.items()}


def split_params(params):
    out, depth, current = [], 0, ''
    for ch in params:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(current.strip())
            current = ''
        else:
            current += ch
    if current.strip():
        out.append(current.strip())
    return [] if out == ['void'] else out


def name_params(params, zero_outputs):
    """Returns (parameter list text, statements zeroing pointer out-parameters)."""
    named, zeroing = [], []
    for index, param in enumerate(split_params(params)):
        if '(' in param or '[' in param:
            named.append(param)
            continue
        tokens = re.findall(r'[A-Za-z_]\w*|\*|&', param)
        unnamed = param.rstrip().endswith(('*', '&')) or len(tokens) < 2 or tokens[-1] in TYPE_WORDS
        if unnamed:
            name = 'p%d' % index
            param = param + ' ' + name
        else:
            name = tokens[-1]
        named.append(param)
        type_part = param[:param.rfind(name)]
        if zero_outputs and '*' in type_part and not type_part.strip().startswith('const') \
                and 'void' not in type_part and type_part.count('*') == 1:
            zeroing.append('    if (%s) *%s = {};' % (name, name))
    return ', '.join(named), zeroing


def main():
    aurora_include, wiiu_dir = sys.argv[1], sys.argv[2]
    headers = load_headers([os.path.join(wiiu_dir, 'include'), aurora_include])
    wanted = [l.strip() for l in open(os.path.join(wiiu_dir, 'stub_symbols.txt')) if l.strip()]
    used_headers, bodies, missing = set(), [], []
    for symbol in wanted:
        if symbol in MANUAL:
            continue
        pattern = re.compile(r'(?:^|[;\n{}])\s*([A-Za-z_][\w\s\*]*?[\s\*])' + re.escape(symbol) +
                             r'\s*\(([^;{]*?)\)\s*;', re.M)
        found = None
        for rel, text in sorted(headers.items()):
            for match in pattern.finditer(text):
                ret = ' '.join(match.group(1).split())
                if 'inline' in ret or 'static' in ret or 'define' in ret:
                    continue
                found = (rel, ret.replace('extern ', ''), ' '.join(match.group(2).split()))
                break
            if found:
                break
        if not found:
            missing.append(symbol)
            continue
        rel, ret, params = found
        if ret != 'void':
            missing.append(symbol + ' (returns %s: add it to aurora_wiiu.cpp)' % ret)
            continue
        used_headers.add(rel)
        params_text, zeroing = name_params(params, symbol.startswith(('GXGet', 'AuroraGet')))
        body = '\n'.join(zeroing)
        bodies.append('void %s(%s) {%s}' % (symbol, params_text, ('\n' + body + '\n') if body else ''))
    if missing:
        print('gen_aurora_stubs: unresolved:\n  ' + '\n  '.join(missing), file=sys.stderr)
        return 1
    out = os.path.join(wiiu_dir, 'src', 'aurora_wiiu_stubs.cpp')
    with open(out, 'w') as f:
        f.write('// SPDX-License-Identifier: GPL-3.0-or-later\n')
        f.write('// GENERATED by wiiu/tools/gen_aurora_stubs.py from aurora\'s headers - do not edit.\n')
        f.write('// Phase 2 (headless) no-op implementations of the aurora/Dolphin-SDK entry points the\n')
        f.write('// runtime links against. Getters zero their out-parameters. Phase 3 replaces these\n')
        f.write('// with the GX2 backend. Value-returning functions live in aurora_wiiu.cpp.\n')
        for rel in sorted(used_headers):
            f.write('#include <%s>\n' % rel)
        f.write('\n// The parameters of no-op stubs are intentionally unused.\n')
        f.write('#pragma GCC diagnostic ignored "-Wunused-parameter"\n\n')
        f.write('\n\n'.join(bodies) + '\n')
    print('gen_aurora_stubs: %d stubs -> %s' % (len(bodies), out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
