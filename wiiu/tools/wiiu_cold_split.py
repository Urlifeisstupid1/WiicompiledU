#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# wiiu_cold_split.py - split the "cold" code region out of a wut ELF.
#
# Background (see PORTING_NOTES.md, T4): under Aroma an RPX may hold only about
# 28 MB of code, but the app also gets a 32 MiB codegen (JIT) area at a fixed
# virtual address (0x01800000 on hardware). Code linked into the `.wiicold`
# output section (placed at that address by wiicold.ld) is moved out of the
# RPX into a separate image that wiicold_loader.c copies into the codegen
# area at startup.
#
# What this tool does to the final ELF (linked with --emit-relocs, as wut does):
#   1. Writes `.wiicold` (code + its .eh_frame) to an image file, together with
#      every relocation inside it that targets an RPX section. The Cafe loader
#      moves the RPX at load time, so those values must be re-based at runtime
#      against "anchor" symbols whose real addresses the loader can take.
#   2. Drops relocations elsewhere in the ELF that point INTO `.wiicold`: the cold
#      region never moves, so the absolute values ld already wrote are final.
#   3. Fails the build on any PC-relative reference crossing between the RPX and
#      the cold region (those cannot survive the RPX being moved; cold code and
#      code that calls it must be compiled with -mlongcall).
#   4. Removes `.wiicold` and `.rela.wiicold` (via objcopy) so elf2rpl never sees
#      them.
#
# Image format (all big-endian):
#   header  : magic 'WCLD', version, link_base, image_size, code_size,
#             eh_frame_offset, eh_frame_size, anchor_count, reloc_count
#   anchors : anchor_count x { u32 link_address }   (order = ANCHORS below)
#   relocs  : reloc_count x { u32 offset, u8 type, u8 anchor, u16 0, u32 link_value }
#   image   : image_size bytes, loaded at link_base

import argparse
import os
import struct
import subprocess
import sys

MAGIC = b'WCLD'
VERSION = 1

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
R_PPC_ADDR24 = 2
R_PPC_ADDR16 = 3
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_REL24 = 10
R_PPC_REL14 = 11
R_PPC_REL32 = 26
R_PPC_EMB_SDA21 = 109
R_PPC_SDAREL16 = 32

ABSOLUTE_TYPES = {R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA}
# Small-data relocations are relative to _SDA_BASE_/_SDA2_BASE_, which the Cafe
# loader moves together with the data they address (r13/r2 are set at runtime),
# so their encoded offsets stay valid without re-basing.
SDA_TYPES = {R_PPC_EMB_SDA21, R_PPC_SDAREL16}
TYPE_NAMES = {
    0: 'NONE', 1: 'ADDR32', 2: 'ADDR24', 3: 'ADDR16', 4: 'ADDR16_LO', 5: 'ADDR16_HI',
    6: 'ADDR16_HA', 10: 'REL24', 11: 'REL14', 26: 'REL32', 32: 'SDAREL16', 109: 'EMB_SDA21',
}


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', help='linked ELF; rewritten in place without .wiicold')
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
    if cold is None:
        raise SplitError('no .wiicold section (is wiicold.ld on the link line?)')
    symbols = read_symbols(data, sections)
    sym_by_name = {}
    for name, value, shndx, _type in symbols:
        sym_by_name.setdefault(name, (value, shndx))

    cold_start, cold_end = cold.addr, cold.addr + cold.size
    if cold.size > args.max_size:
        raise SplitError('.wiicold is %d bytes, more than the %d-byte codegen area'
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

    eh = sym_by_name.get('__wiicold_eh_frame_start')
    eh_end = sym_by_name.get('__wiicold_eh_frame_end')
    eh_offset = (eh[0] - cold_start) if eh else 0
    eh_size = (eh_end[0] - eh[0]) if eh and eh_end else 0
    code_end = sym_by_name.get('__wiicold_code_end')
    code_size = (code_end[0] - cold_start) if code_end else cold.size

    image_relocs = []
    errors = []

    def describe(sym_index):
        name, value, shndx, _ = symbols[sym_index]
        return '%s (0x%08X)' % (name or '<section %d>' % shndx, value)

    for rela in [s for s in sections if s.type == SHT_RELA]:
        target = sections[rela.info]
        entries = []
        for i in range(rela.size // 12):
            r_offset, r_info, r_addend = struct.unpack_from('>IIi', data, rela.offset + i * 12)
            entries.append((r_offset, r_info, r_addend))
        kept = []
        for r_offset, r_info, r_addend in entries:
            r_type = r_info & 0xFF
            r_sym = r_info >> 8
            name, s_value, s_shndx, _ = symbols[r_sym]
            s_in_cold = s_shndx == cold.index
            if target.index == cold.index:
                # Relocation inside the cold image.
                if r_type == R_PPC_NONE or s_shndx in (cold.index, SHN_ABS) or r_type in SDA_TYPES:
                    continue  # final already: cold never moves / SDA offsets are invariant
                if s_shndx == SHN_UNDEF:
                    if r_type in ABSOLUTE_TYPES and name == '':
                        continue
                    errors.append('cold+0x%X: %s against undefined %s'
                                  % (r_offset - cold_start, TYPE_NAMES.get(r_type, r_type), describe(r_sym)))
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
                image_relocs.append((r_offset - cold_start, r_type, anchor,
                                     (s_value + r_addend) & 0xFFFFFFFF))
                continue
            if rela.index == by_name.get('.rela.wiicold', cold).index:
                continue
            if s_in_cold or (s_shndx == SHN_ABS and cold_start <= s_value < cold_end):
                if not (target.flags & SHF_ALLOC):
                    continue  # debug info etc.: elf2rpl drops it anyway
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

    if errors:
        for e in errors[:50]:
            print('wiiu_cold_split: error: ' + e, file=sys.stderr)
        if len(errors) > 50:
            print('wiiu_cold_split: ... %d more' % (len(errors) - 50), file=sys.stderr)
        return 1

    image = bytes(data[cold.offset:cold.offset + cold.size]) if cold.type != SHT_NOBITS else bytes(cold.size)
    os.makedirs(os.path.dirname(os.path.abspath(args.image)), exist_ok=True)
    with open(args.image, 'wb') as out:
        out.write(MAGIC)
        out.write(struct.pack('>8I', VERSION, cold_start, len(image), code_size, eh_offset, eh_size,
                              len(anchor_links), len(image_relocs)))
        for link in anchor_links:
            out.write(struct.pack('>I', link))
        for offset, r_type, anchor, value in image_relocs:
            out.write(struct.pack('>IBBHI', offset, r_type, anchor, 0, value))
        out.write(image)

    open(args.elf, 'wb').write(data)
    subprocess.check_call([args.objcopy, '--remove-section=.rela.wiicold',
                           '--remove-section=.wiicold', args.elf])
    print('wiiu_cold_split: %d bytes of cold code+eh_frame at 0x%08X, %d runtime relocations -> %s'
          % (len(image), cold_start, len(image_relocs), args.image))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except SplitError as error:
        print('wiiu_cold_split: error: %s' % error, file=sys.stderr)
        sys.exit(1)
