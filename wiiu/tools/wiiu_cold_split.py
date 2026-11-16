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
# The cold .eh_frame is NOT fixed up from the emitted relocations: ld rewrites
# .eh_frame (merging duplicate CIEs) but --emit-relocs keeps the offsets from
# before that rewrite, so they point at the wrong bytes (found on hardware:
# relocations past the end of the image). Instead the tool parses the CIEs/FDEs
# in the final image and records the absolute pointers (personality routine,
# LSDA) that refer to RPX sections.
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


DW_EH_PE_omit = 0xFF


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


def scan_eh_frame(image, base, start, end):
    """Yields (offset_in_image, target_address, is_absolute) for every pointer field
    (personality, FDE pc_begin, LSDA) in the eh_frame bytes image[start:end]."""
    cies = {}
    pos = start
    while pos + 4 <= end:
        (length,) = struct.unpack_from('>I', image, pos)
        if length == 0:
            break
        if length == 0xFFFFFFFF:
            raise SplitError('64-bit eh_frame entries are not supported')
        body = pos + 4
        next_pos = body + length
        (cie_id,) = struct.unpack_from('>I', image, body)
        if cie_id == 0:
            q = body + 4
            version = image[q]
            q += 1
            aug_end = image.index(b'\0', q)
            aug = image[q:aug_end].decode('ascii')
            q = aug_end + 1
            _, q = read_uleb(image, q)  # code alignment
            _, q = read_sleb(image, q)  # data alignment
            if version == 1:
                q += 1
            else:
                _, q = read_uleb(image, q)
            info = {'aug': aug, 'fde_enc': 0x00, 'lsda_enc': DW_EH_PE_omit}
            if aug.startswith('z'):
                _, q = read_uleb(image, q)
                for ch in aug[1:]:
                    if ch == 'P':
                        enc = image[q]
                        q += 1
                        value, size, absolute = read_encoded(image, q, enc, base + q)
                        yield q, value, absolute
                        q += size
                    elif ch == 'L':
                        info['lsda_enc'] = image[q]
                        q += 1
                    elif ch == 'R':
                        info['fde_enc'] = image[q]
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
                raise SplitError('FDE at cold+0x%X refers to an unknown CIE' % pos)
            info = cies[cie_pos]
            q = body + 4
            value, size, absolute = read_encoded(image, q, info['fde_enc'], base + q)
            yield q, value, absolute
            q += size
            q += 4  # pc_range (same size, never relocated)
            if info['aug'].startswith('z'):
                _, q = read_uleb(image, q)
                if 'L' in info['aug'] and info['lsda_enc'] != DW_EH_PE_omit:
                    value, size, absolute = read_encoded(image, q, info['lsda_enc'], base + q)
                    if value != 0 or not absolute:
                        yield q, value, absolute
        pos = next_pos


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
                # Relocation inside the cold image. Those at or past the start of the
                # cold .eh_frame have stale offsets (see header) and are replaced by
                # scan_eh_frame() below.
                if eh and r_offset >= eh[0]:
                    continue
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

    image = bytes(data[cold.offset:cold.offset + cold.size]) if cold.type != SHT_NOBITS else bytes(cold.size)

    if eh_size:
        for offset, value, absolute in scan_eh_frame(image, cold_start, eh_offset, eh_offset + eh_size):
            in_cold = cold_start <= value < cold_end
            if in_cold:
                continue  # PC-relative or absolute reference to fixed cold code: final
            if not absolute:
                errors.append('cold eh_frame+0x%X: PC-relative pointer to 0x%08X outside the cold region'
                              % (offset - eh_offset, value))
                continue
            owner = section_containing(sections, value)
            anchor = anchor_of_section.get(owner.index) if owner else None
            if anchor is None:
                errors.append('cold eh_frame+0x%X: pointer to 0x%08X in %s, which has no anchor'
                              % (offset - eh_offset, value, owner.name if owner else 'no section'))
                continue
            image_relocs.append((offset, R_PPC_ADDR32, anchor, value))

    for offset, r_type, _anchor, _value in image_relocs:
        width = 4 if r_type == R_PPC_ADDR32 else 2
        if offset + width > len(image):
            errors.append('relocation at cold+0x%X (%s) is outside the %d-byte image'
                          % (offset, TYPE_NAMES.get(r_type, r_type), len(image)))

    if errors:
        for e in errors[:50]:
            print('wiiu_cold_split: error: ' + e, file=sys.stderr)
        if len(errors) > 50:
            print('wiiu_cold_split: ... %d more' % (len(errors) - 50), file=sys.stderr)
        return 1

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
