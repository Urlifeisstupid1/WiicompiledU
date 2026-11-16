#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# wiiu_cold_split.py - split the "cold" code out of a wut ELF (PORTING_NOTES.md, D8).
#
# Under Aroma an RPX may hold only about 28 MB of code, but the app also gets a
# 32 MiB codegen (JIT) area at a fixed virtual address (0x01800000 on hardware,
# executable on the codegen core only). wiicold.ld links the code of the cold
# libraries into `.wiicold` at that address and their .eh_frame into
# `.wiicold_eh` at an arbitrary address. This tool moves both out of the RPX into
# cold.bin, which wiicold_loader.c loads at startup: the code into the codegen
# area, the unwind tables into heap memory.
#
# What it does to the final ELF (linked with --emit-relocs, as wut does):
#   1. Writes the cold code, plus every relocation inside it that targets an RPX
#      section. The Cafe loader moves the RPX, so those values are re-based at
#      runtime against "anchor" symbols whose real addresses the loader can take.
#   2. Writes the cold .eh_frame, parsed directly (CIE personality, FDE pc_begin,
#      LSDA) rather than from ld's emitted relocations: ld rewrites .eh_frame but
#      --emit-relocs keeps the pre-rewrite offsets (found on hardware). Pointers to
#      cold code are rewritten by the loader for the table's real heap address;
#      absolute pointers into the RPX are re-based like code relocations.
#   3. Neutralises FDEs that describe a function outside the cold region (range
#      zero, never matches a PC). They come from inline/template functions compiled
#      into both cold and RPX objects: the linker keeps the RPX copy, which has its
#      own FDE in the main .eh_frame, but leaves the cold object's FDE behind.
#   4. Drops relocations elsewhere in the ELF that point into `.wiicold` (cold code
#      never moves, so the absolute values ld wrote are final) and fails the build
#      on any PC-relative reference crossing between the RPX and the cold region
#      (cold code and its callers must use -mlongcall; -Os register save/restore
#      helpers are handled by WiiUCold.cmake).
#   5. Removes the cold sections (objcopy) so elf2rpl never sees them.
#
# cold.bin, version 2 (all big-endian):
#   header      : 'WCLD', version, link_base, code_size, eh_size, anchor_count,
#                 code_reloc_count, eh_reloc_count
#   anchors     : anchor_count x u32 link address (order = ANCHORS)
#   code relocs : code_reloc_count x { u32 offset, u8 type, u8 anchor, u16 0, u32 value }
#   eh relocs   : eh_reloc_count x (same layout; offsets are relative to the eh data)
#   code        : code_size bytes, loaded at link_base
#   eh data     : eh_size bytes, loaded anywhere (heap) and registered with libgcc
# Relocation types: R_PPC_ADDR32/ADDR16_LO/ADDR16_HI/ADDR16_HA with value = link
# address of the RPX target (re-based by anchor), and R_WIICOLD_PCREL32_COLD with
# value = absolute address of a cold-code target (written as target - field address).

import argparse
import os
import struct
import subprocess
import sys

MAGIC = b'WCLD'
VERSION = 2

# Must match wiicold_loader.c. Each anchor lives in the named RPX output section.
ANCHORS = [
    ('wiicold_anchor_text', '.text'),
    ('wiicold_anchor_rodata', '.rodata'),
    ('wiicold_anchor_data', '.data'),
    ('wiicold_anchor_bss', '.bss'),
]

SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHF_ALLOC = 0x2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1

R_PPC_NONE = 0
R_PPC_ADDR32 = 1
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_SDAREL16 = 32
R_PPC_EMB_SDA21 = 109
R_WIICOLD_PCREL32_COLD = 200

ABSOLUTE_TYPES = {R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA}
# Small-data relocations are relative to _SDA_BASE_/_SDA2_BASE_, which the Cafe
# loader moves together with the data they address (r13/r2 are set at runtime),
# so their encoded offsets stay valid without re-basing.
SDA_TYPES = {R_PPC_EMB_SDA21, R_PPC_SDAREL16}
TYPE_NAMES = {
    0: 'NONE', 1: 'ADDR32', 2: 'ADDR24', 3: 'ADDR16', 4: 'ADDR16_LO', 5: 'ADDR16_HI',
    6: 'ADDR16_HA', 10: 'REL24', 11: 'REL14', 26: 'REL32', 32: 'SDAREL16', 109: 'EMB_SDA21',
}
DW_EH_PE_omit = 0xFF


class SplitError(Exception):
    pass


class Section:
    __slots__ = ('index', 'name', 'type', 'flags', 'addr', 'offset', 'size', 'link', 'info',
                 'entsize', 'header_offset')


def read_elf(data):
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 2:
        raise SplitError('not a 32-bit big-endian ELF')
    (e_shoff,) = struct.unpack_from('>I', data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('>HHH', data, 0x2E)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        s = Section()
        (name, s.type, s.flags, s.addr, s.offset, s.size, s.link, s.info, _align,
         s.entsize) = struct.unpack_from('>10I', data, off)
        s.index = i
        s.name = name
        s.header_offset = off
        sections.append(s)
    shstr = sections[e_shstrndx]
    for s in sections:
        end = data.index(b'\0', shstr.offset + s.name)
        s.name = data[shstr.offset + s.name:end].decode('ascii')
    return sections


def read_symbols(data, sections):
    symtab = next((s for s in sections if s.type == SHT_SYMTAB), None)
    if symtab is None:
        raise SplitError('ELF has no symbol table')
    strtab = sections[symtab.link]
    symbols = []
    for i in range(symtab.size // 16):
        name_off, value, size, info, other, shndx = struct.unpack_from(
            '>IIIBBH', data, symtab.offset + i * 16)
        end = data.index(b'\0', strtab.offset + name_off)
        name = data[strtab.offset + name_off:end].decode('ascii', 'replace')
        symbols.append((name, value, shndx, info & 0xF))
    return symbols


def section_containing(sections, address):
    for s in sections:
        if s.flags & SHF_ALLOC and s.addr <= address < s.addr + max(s.size, 1):
            return s
    return None


def section_bytes(data, section):
    if section.type == SHT_NOBITS:
        return bytes(section.size)
    return bytes(data[section.offset:section.offset + section.size])


def read_uleb(buf, pos):
    result = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, pos


def read_sleb(buf, pos):
    result = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            if byte & 0x40:
                result -= 1 << shift
            return result, pos


def read_encoded(buf, pos, encoding, field_address):
    """Returns (value, size, is_absolute) for a 4-byte DW_EH_PE pointer."""
    fmt = encoding & 0x0F
    app = encoding & 0x70
    if encoding & 0x80:
        raise SplitError('indirect eh_frame pointer encoding 0x%02X is not supported' % encoding)
    if fmt not in (0x00, 0x03, 0x0B):
        raise SplitError('eh_frame pointer encoding 0x%02X is not supported' % encoding)
    (raw,) = struct.unpack_from('>i' if fmt == 0x0B else '>I', buf, pos)
    if app == 0x00:
        return raw & 0xFFFFFFFF, 4, True
    if app == 0x10:
        return (field_address + raw) & 0xFFFFFFFF, 4, False
    raise SplitError('eh_frame pointer application 0x%02X is not supported' % app)


def scan_eh_frame(eh, eh_link):
    """Yields (kind, offset, target, is_absolute, fde_target) for every pointer field in the
    .eh_frame bytes `eh` linked at `eh_link`. kind is 'personality', 'pc_begin' or 'lsda'; a
    'pc_begin' field is followed by its 4-byte pc_range. fde_target is the FDE's own
    pc_begin target (for 'pc_begin' and 'lsda'), else None."""
    cies = {}
    pos = 0
    while pos + 4 <= len(eh):
        (length,) = struct.unpack_from('>I', eh, pos)
        if length == 0:
            break
        if length == 0xFFFFFFFF:
            raise SplitError('64-bit eh_frame entries are not supported')
        body = pos + 4
        next_pos = body + length
        (cie_id,) = struct.unpack_from('>I', eh, body)
        if cie_id == 0:
            q = body + 4
            version = eh[q]
            q += 1
            aug_end = eh.index(b'\0', q)
            aug = eh[q:aug_end].decode('ascii')
            q = aug_end + 1
            _, q = read_uleb(eh, q)  # code alignment
            _, q = read_sleb(eh, q)  # data alignment
            if version == 1:
                q += 1
            else:
                _, q = read_uleb(eh, q)
            info = {'aug': aug, 'fde_enc': 0x00, 'lsda_enc': DW_EH_PE_omit}
            if aug.startswith('z'):
                _, q = read_uleb(eh, q)
                for ch in aug[1:]:
                    if ch == 'P':
                        enc = eh[q]
                        q += 1
                        value, size, absolute = read_encoded(eh, q, enc, eh_link + q)
                        yield 'personality', q, value, absolute, None
                        q += size
                    elif ch == 'L':
                        info['lsda_enc'] = eh[q]
                        q += 1
                    elif ch == 'R':
                        info['fde_enc'] = eh[q]
                        q += 1
                    elif ch in 'SB':
                        pass
                    else:
                        raise SplitError('unknown CIE augmentation %r' % aug)
            elif aug:
                raise SplitError('unsupported CIE augmentation %r' % aug)
            cies[pos] = info
        else:
            cie_pos = body - cie_id
            if cie_pos not in cies:
                raise SplitError('FDE at eh+0x%X refers to an unknown CIE' % pos)
            info = cies[cie_pos]
            q = body + 4
            fde_target, size, absolute = read_encoded(eh, q, info['fde_enc'], eh_link + q)
            yield 'pc_begin', q, fde_target, absolute, fde_target
            q += size + 4  # pc_begin, pc_range
            if info['aug'].startswith('z'):
                _, q = read_uleb(eh, q)
                if 'L' in info['aug'] and info['lsda_enc'] != DW_EH_PE_omit:
                    value, size, absolute = read_encoded(eh, q, info['lsda_enc'], eh_link + q)
                    if value != 0 or not absolute:
                        yield 'lsda', q, value, absolute, fde_target
        pos = next_pos


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', help='linked ELF; rewritten in place without the cold sections')
    parser.add_argument('image', help='output cold image (e.g. content/cold.bin)')
    parser.add_argument('--objcopy', default='powerpc-eabi-objcopy')
    parser.add_argument('--keep-full', help='also save the unmodified ELF here')
    parser.add_argument('--max-size', type=lambda v: int(v, 0), default=0x02000000)
    args = parser.parse_args()

    data = bytearray(open(args.elf, 'rb').read())
    if args.keep_full:
        open(args.keep_full, 'wb').write(data)
    sections = read_elf(data)
    by_name = {s.name: s for s in sections}
    cold = by_name.get('.wiicold')
    cold_eh = by_name.get('.wiicold_eh')
    if cold is None or cold_eh is None:
        raise SplitError('no .wiicold/.wiicold_eh sections (is the WiiUCold linker script in use?)')
    symbols = read_symbols(data, sections)
    sym_by_name = {}
    for name, value, shndx, _type in symbols:
        sym_by_name.setdefault(name, (value, shndx))

    cold_start, cold_end = cold.addr, cold.addr + cold.size
    if cold.size > args.max_size:
        raise SplitError('cold code is %d bytes, more than the %d-byte codegen area'
                         % (cold.size, args.max_size))

    # Anchors: one symbol per RPX output section the cold code may reference.
    anchor_links = []
    anchor_of_section = {}
    for i, (sym, secname) in enumerate(ANCHORS):
        if sym not in sym_by_name:
            raise SplitError('anchor %s missing (link wiicold_loader.c)' % sym)
        value, shndx = sym_by_name[sym]
        if sections[shndx].name != secname:
            raise SplitError('anchor %s is in %s, expected %s'
                             % (sym, sections[shndx].name, secname))
        anchor_links.append(value)
        anchor_of_section[shndx] = i

    code_relocs = []
    eh_relocs = []
    errors = []

    def describe(sym_index):
        name, value, shndx, _ = symbols[sym_index]
        return '%s (0x%08X)' % (name or '<section %d>' % shndx, value)

    for rela in [s for s in sections if s.type == SHT_RELA]:
        target = sections[rela.info]
        entries = [struct.unpack_from('>IIi', data, rela.offset + i * 12) for i in range(rela.size // 12)]
        kept = []
        for r_offset, r_info, r_addend in entries:
            r_type = r_info & 0xFF
            r_sym = r_info >> 8
            name, s_value, s_shndx, _ = symbols[r_sym]
            if target.index == cold_eh.index:
                continue  # stale after ld's .eh_frame rewrite; scan_eh_frame() replaces these
            if target.index == cold.index:
                if r_type == R_PPC_NONE or s_shndx in (cold.index, SHN_ABS) or r_type in SDA_TYPES:
                    continue  # final already: cold never moves / SDA offsets are invariant
                if s_shndx == SHN_UNDEF:
                    if r_type in ABSOLUTE_TYPES and name == '':
                        continue
                    errors.append('cold+0x%X: %s against undefined %s'
                                  % (r_offset - cold_start, TYPE_NAMES.get(r_type, r_type), describe(r_sym)))
                    continue
                if s_shndx == cold_eh.index:
                    errors.append('cold+0x%X: code references the cold .eh_frame: %s'
                                  % (r_offset - cold_start, describe(r_sym)))
                    continue
                if r_type not in ABSOLUTE_TYPES:
                    errors.append('cold+0x%X: PC-relative %s into the RPX: %s (compile cold code with -mlongcall)'
                                  % (r_offset - cold_start, TYPE_NAMES.get(r_type, r_type), describe(r_sym)))
                    continue
                anchor = anchor_of_section.get(s_shndx)
                if anchor is None:
                    errors.append('cold+0x%X: reference into RPX section %s, which has no anchor: %s'
                                  % (r_offset - cold_start, sections[s_shndx].name, describe(r_sym)))
                    continue
                code_relocs.append((r_offset - cold_start, r_type, anchor, (s_value + r_addend) & 0xFFFFFFFF))
                continue
            if s_shndx in (cold.index, cold_eh.index) or \
                    (s_shndx == SHN_ABS and cold_start <= s_value < cold_end):
                if not (target.flags & SHF_ALLOC):
                    continue  # debug info etc.: elf2rpl drops it anyway
                if s_shndx == cold_eh.index:
                    errors.append('%s+0x%X: the RPX references the cold .eh_frame (%s), which moves at runtime'
                                  % (target.name, r_offset - target.addr, describe(r_sym)))
                    continue
                if r_type in ABSOLUTE_TYPES:
                    continue  # absolute address of fixed cold code: already final
                errors.append('%s+0x%X: PC-relative %s into the cold region: %s (compile with -mlongcall)'
                              % (target.name, r_offset - target.addr, TYPE_NAMES.get(r_type, r_type),
                                 describe(r_sym)))
                continue
            kept.append((r_offset, r_info, r_addend))
        if len(kept) != len(entries):
            for i, entry in enumerate(kept):
                struct.pack_into('>IIi', data, rela.offset + i * 12, *entry)
            struct.pack_into('>I', data, rela.header_offset + 0x14, len(kept) * 12)

    code = section_bytes(data, cold)
    eh = bytearray(section_bytes(data, cold_eh))
    neutralised = 0
    for kind, offset, value, absolute, fde_target in list(scan_eh_frame(bytes(eh), cold_eh.addr)):
        fde_in_cold = fde_target is not None and cold_start <= fde_target < cold_end
        if kind == 'pc_begin':
            if not fde_in_cold:
                # Describes a function the linker kept in the RPX: make it cover nothing.
                value = cold_start
                struct.pack_into('>I', eh, offset + 4, 0)
                neutralised += 1
            if absolute:
                struct.pack_into('>I', eh, offset, value)  # fixed cold address: final
            else:
                eh_relocs.append((offset, R_WIICOLD_PCREL32_COLD, 0, value))
            continue
        if kind == 'lsda' and not fde_in_cold:
            continue  # belongs to a neutralised FDE
        if cold_start <= value < cold_end:
            if absolute:
                continue  # fixed cold address: final
            eh_relocs.append((offset, R_WIICOLD_PCREL32_COLD, 0, value))
            continue
        if not absolute:
            errors.append('cold eh_frame+0x%X: PC-relative %s pointer to 0x%08X outside the cold code'
                          % (offset, kind, value))
            continue
        owner = section_containing(sections, value)
        anchor = anchor_of_section.get(owner.index) if owner else None
        if anchor is None:
            errors.append('cold eh_frame+0x%X: %s pointer to 0x%08X in %s, which has no anchor'
                          % (offset, kind, value, owner.name if owner else 'no section'))
            continue
        eh_relocs.append((offset, R_PPC_ADDR32, anchor, value))

    for what, relocs, size in (('code', code_relocs, len(code)), ('eh_frame', eh_relocs, len(eh))):
        for offset, r_type, _anchor, _value in relocs:
            width = 2 if r_type in (R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA) else 4
            if offset + width > size:
                errors.append('%s relocation at +0x%X (%s) is outside the %d-byte section'
                              % (what, offset, TYPE_NAMES.get(r_type, r_type), size))

    if errors:
        for e in errors[:50]:
            print('wiiu_cold_split: error: ' + e, file=sys.stderr)
        if len(errors) > 50:
            print('wiiu_cold_split: ... %d more' % (len(errors) - 50), file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.image)), exist_ok=True)
    with open(args.image, 'wb') as out:
        out.write(MAGIC)
        out.write(struct.pack('>7I', VERSION, cold_start, len(code), len(eh), len(anchor_links),
                              len(code_relocs), len(eh_relocs)))
        for link in anchor_links:
            out.write(struct.pack('>I', link))
        for offset, r_type, anchor, value in code_relocs + eh_relocs:
            out.write(struct.pack('>IBBHI', offset, r_type, anchor, 0, value))
        out.write(code)
        out.write(bytes(eh))

    open(args.elf, 'wb').write(data)
    subprocess.check_call([args.objcopy, '--remove-section=.rela.wiicold', '--remove-section=.wiicold',
                           '--remove-section=.rela.wiicold_eh', '--remove-section=.wiicold_eh', args.elf])
    print('wiiu_cold_split: %d bytes of cold code at 0x%08X (%d relocations), %d bytes of unwind tables '
          '(%d relocations, %d duplicate FDEs neutralised) -> %s'
          % (len(code), cold_start, len(code_relocs), len(eh), len(eh_relocs), neutralised, args.image))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except SplitError as error:
        print('wiiu_cold_split: error: %s' % error, file=sys.stderr)
        sys.exit(1)
