#!/usr/bin/env python3
"""
papp_pack.py — turn a PAPP ELF (linked at 0 with -Wl,-q) into a .papp v2 image.

  python papp_pack.py app.elf app.papp

The ELF must be linked with papp-sdk/ld/papp.ld and --emit-relocs (-Wl,-q).
Section sizes, entry, import table and manifest come from the ELF, so nothing
has to be passed by hand. Relocation rules: docs/ARCHITECTURE.md §5.4.

Also importable: build_image(elf_path) -> PappImage, relocate(image, base, fast_base) for tests.
"""

import argparse
import struct
import sys
import zlib
from dataclasses import dataclass, field

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
from elftools.elf.relocation import RelocationSection

MAGIC = 0x32504150          # "PAP2"
FMT_VERSION = 1
HEADER_SIZE = 64
MANIFEST_MAGIC = 0x464E414D
IMPORT_SIZE = 12

R_ABS32, R_HI20, R_LO12_I, R_LO12_S = 1, 2, 3, 4
R_PCREL_HI20, R_PCREL_LO12_I, R_PCREL_LO12_S = 5, 6, 7
R_DIFF32 = 8                        # 32-bit label difference across regions (jump tables)
SITE_FAST = 1 << 27
VAL_FAST = 1 << 31
FAST_LINK_BASE = 0x10000000         # papp.ld: where the FAST region is linked

# RISC-V ELF relocation numbers
RV = dict(R_RISCV_NONE=0, R_RISCV_32=1, R_RISCV_BRANCH=16, R_RISCV_JAL=17, R_RISCV_CALL=18,
          R_RISCV_CALL_PLT=19, R_RISCV_GOT_HI20=20, R_RISCV_PCREL_HI20=23, R_RISCV_PCREL_LO12_I=24,
          R_RISCV_PCREL_LO12_S=25, R_RISCV_HI20=26, R_RISCV_LO12_I=27, R_RISCV_LO12_S=28,
          R_RISCV_ALIGN=43, R_RISCV_RVC_BRANCH=44, R_RISCV_RVC_JUMP=45, R_RISCV_RVC_LUI=46,
          R_RISCV_ADD32=35, R_RISCV_SUB32=39, R_RISCV_RELAX=51, R_RISCV_32_PCREL=57, R_RISCV_SET_ULEB128=60, R_RISCV_SUB_ULEB128=61)
ABSOLUTE = {RV["R_RISCV_32"]: R_ABS32, RV["R_RISCV_HI20"]: R_HI20,
            RV["R_RISCV_LO12_I"]: R_LO12_I, RV["R_RISCV_LO12_S"]: R_LO12_S}
PC_RELATIVE = {RV[n] for n in ("R_RISCV_BRANCH", "R_RISCV_JAL", "R_RISCV_CALL", "R_RISCV_CALL_PLT",
                               "R_RISCV_PCREL_HI20", "R_RISCV_PCREL_LO12_I", "R_RISCV_PCREL_LO12_S",
                               "R_RISCV_RVC_BRANCH", "R_RISCV_RVC_JUMP", "R_RISCV_32_PCREL")}
# Label differences and linker hints: position independent, nothing to do
IGNORED = {RV["R_RISCV_NONE"], RV["R_RISCV_ALIGN"], RV["R_RISCV_RELAX"],
           RV["R_RISCV_SET_ULEB128"], RV["R_RISCV_SUB_ULEB128"]} | set(range(33, 43)) | set(range(52, 56))

OPC_LUI, OPC_AUIPC = 0x37, 0x17


class PackError(Exception):
    pass


@dataclass
class PappImage:
    image: bytearray
    bss_size: int
    entry_off: int
    imports_off: int
    imports_count: int
    manifest_off: int
    fast: bytearray = field(default_factory=bytearray)
    fast_bss_size: int = 0
    relocs: list = field(default_factory=list)   # (offset|SITE_FAST, type, value|VAL_FAST), load order
    manifest: dict = field(default_factory=dict)
    abs_fixups: list = field(default_factory=list)  # (auipc_off, lo12_offs, value) rewritten to lui


def _patch_lo12(image, off, rtype, lo):
    insn = struct.unpack_from("<I", image, off)[0]
    if rtype == RV["R_RISCV_PCREL_LO12_S"]:
        insn = (insn & 0x01FFF07F) | ((lo >> 5) << 25) | ((lo & 0x1F) << 7)
    else:
        insn = (insn & 0x000FFFFF) | (lo << 20)
    struct.pack_into("<I", image, off, insn)


def _rewrite_abs_pcrel(image, abs_pcrel, lo12_sites, image_end):
    """auipc rd,X / addi rd,rd,Y aimed at an absolute address → lui rd,%hi / addi rd,rd,%lo."""
    fixups = []
    for hi_off, value in sorted(abs_pcrel.items()):
        insn = struct.unpack_from("<I", image, hi_off)[0]
        if insn & 0x7F == OPC_LUI:
            continue                                # ld already made it absolute
        if insn & 0x7F != OPC_AUIPC or hi_off + 4 > image_end:
            raise PackError(f"unexpected instruction {insn:#010x} at {hi_off:#x} for PCREL_HI20")
        hi = ((value + 0x800) >> 12) & 0xFFFFF
        struct.pack_into("<I", image, hi_off, (hi << 12) | (insn & 0xF80) | OPC_LUI)
        los = [(o, t) for o, t, label in lo12_sites if label == hi_off]
        for o, t in los:
            _patch_lo12(image, o, t, value & 0xFFF)
        fixups.append((hi_off, [o for o, _ in los], value))
    return fixups


def _symbols(elf):
    symtab = elf.get_section_by_name(".symtab")
    if not isinstance(symtab, SymbolTableSection):
        raise PackError("ELF has no symbol table (don't strip it)")
    return symtab


def _tag(addr, flag):
    """Link address -> region offset, with `flag` set for the FAST region."""
    return (addr - FAST_LINK_BASE) | flag if addr >= FAST_LINK_BASE else addr


def build_image(elf_path):
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        if elf["e_machine"] != "EM_RISCV" or elf.elfclass != 32:
            raise PackError("not a 32-bit RISC-V ELF")
        symtab = _symbols(elf)
        syms = {s.name: s for s in symtab.iter_symbols() if s.name}

        def sym(name):
            if name not in syms:
                raise PackError(f"missing symbol {name} (link with papp-sdk/ld/papp.ld)")
            return syms[name]["st_value"]

        image_end = sym("__papp_image_end")
        bss_end = sym("__bss_end")
        if image_end == 0 or bss_end < image_end:
            raise PackError("bad image layout")
        # FAST bounds from the section headers: ld drops an empty .fast, and its
        # marker symbols then land somewhere in SLOW
        fast_end = fast_bss_end = FAST_LINK_BASE
        for sec in elf.iter_sections():
            if sec["sh_flags"] & 0x2 and sec["sh_size"] and sec["sh_addr"] >= FAST_LINK_BASE:
                end = sec["sh_addr"] + sec["sh_size"]
                if sec["sh_type"] == "SHT_NOBITS":
                    fast_bss_end = max(fast_bss_end, end)
                else:
                    fast_end = max(fast_end, end)
        fast_bss_end = max(fast_bss_end, fast_end)
        if bss_end >= FAST_LINK_BASE:
            raise PackError("SLOW region reaches the FAST link address")

        # Flat images of both regions from every allocated PROGBITS section
        image = bytearray(image_end)
        fast = bytearray(fast_end - FAST_LINK_BASE)
        for sec in elf.iter_sections():
            if not (sec["sh_flags"] & 0x2) or sec["sh_size"] == 0:      # SHF_ALLOC
                continue
            lo, hi = sec["sh_addr"], sec["sh_addr"] + sec["sh_size"]
            in_fast = lo >= FAST_LINK_BASE
            if sec["sh_type"] == "SHT_NOBITS":
                ok = (fast_end <= lo and hi <= fast_bss_end) if in_fast else (image_end <= lo and hi <= bss_end)
                if not ok:
                    raise PackError(f"NOBITS section {sec.name} outside .bss / .fast_bss")
                continue
            if in_fast:
                if hi > fast_end:
                    raise PackError(f"section {sec.name} [{lo:#x},{hi:#x}) is past the FAST image end")
                fast[lo - FAST_LINK_BASE:hi - FAST_LINK_BASE] = sec.data()
            else:
                if hi > image_end:
                    raise PackError(f"section {sec.name} [{lo:#x},{hi:#x}) is past the image end {image_end:#x}")
                image[lo:hi] = sec.data()

        def region(addr):
            return addr >= FAST_LINK_BASE

        def in_image(addr):
            return addr <= bss_end or FAST_LINK_BASE <= addr <= fast_bss_end

        def stored(addr):
            return addr + 4 <= image_end or FAST_LINK_BASE <= addr <= fast_end - 4

        # Relocations from --emit-relocs
        relocs = []
        unsupported = {}
        abs_pcrel = {}      # auipc address -> absolute target
        lo12_sites = []
        cross = {}          # auipc address -> target, for pc-relative pairs crossing regions
        diffs = {}          # site -> [added target, subtracted target] (ADD32/SUB32 pairs)
        for rsec in elf.iter_sections():
            if not isinstance(rsec, RelocationSection) or rsec["sh_type"] != "SHT_RELA":
                continue
            target = elf.get_section(rsec["sh_info"])
            if not (target["sh_flags"] & 0x2):
                continue                            # debug info etc.
            rsymtab = elf.get_section(rsec["sh_link"])
            for r in rsec.iter_relocations():
                rtype = r["r_info_type"]
                if rtype in (RV["R_RISCV_ADD32"], RV["R_RISCV_SUB32"]):
                    s = rsymtab.get_symbol(r["r_info_sym"])
                    pair = diffs.setdefault(r["r_offset"], [None, None])
                    pair[rtype == RV["R_RISCV_SUB32"]] = (s["st_value"] + r["r_addend"]) & 0xFFFFFFFF
                    continue
                if rtype in IGNORED:
                    continue
                s = rsymtab.get_symbol(r["r_info_sym"])
                shndx = s["st_shndx"]
                S = s["st_value"]
                A = r["r_addend"]
                off = r["r_offset"]                 # ET_EXEC: virtual address
                absolute_target = shndx == "SHN_ABS" or (shndx == "SHN_UNDEF")
                if rtype in ABSOLUTE:
                    if absolute_target:
                        continue                    # constant / undefined weak: stays as linked
                    if not stored(off):
                        raise PackError(f"reloc at {off:#x} outside the stored image ({target.name})")
                    value = (S + A) & 0xFFFFFFFF
                    if not in_image(value):
                        raise PackError(f"reloc at {off:#x} targets {value:#x} beyond the image")
                    if rtype == RV["R_RISCV_32"]:
                        buf, o = (fast, off - FAST_LINK_BASE) if region(off) else (image, off)
                        have = struct.unpack_from("<I", buf, o)[0]
                        if have != value:
                            raise PackError(f"R_RISCV_32 at {off:#x}: word {have:#x} != S+A {value:#x}")
                    relocs.append((off, ABSOLUTE[rtype], value))
                elif rtype in PC_RELATIVE:
                    tgt = (S + A) & 0xFFFFFFFF
                    if rtype == RV["R_RISCV_PCREL_HI20"] and absolute_target:
                        # auipc-based reference to an absolute address (typically an undefined
                        # weak symbol = 0, e.g. `if (weak_fn)`): pc-relative would move with
                        # the base, so it is rewritten to lui/addi below.
                        abs_pcrel[off] = tgt
                    elif rtype in (RV["R_RISCV_PCREL_LO12_I"], RV["R_RISCV_PCREL_LO12_S"]):
                        lo12_sites.append((off, rtype, S))    # S = address of the paired auipc
                    elif not absolute_target and region(off) != region(tgt):
                        # Crossing SLOW <-> FAST: the distance changes at load time
                        if rtype == RV["R_RISCV_PCREL_HI20"]:
                            cross[off] = tgt
                        elif rtype in (RV["R_RISCV_CALL"], RV["R_RISCV_CALL_PLT"]):
                            cross[off] = tgt                  # auipc ra + jalr ra at off + 4
                            lo12_sites.append((off + 4, RV["R_RISCV_PCREL_LO12_I"], off))
                        else:
                            name = next((k for k, v in RV.items() if v == rtype), str(rtype))
                            raise PackError(f"{name} at {off:#x} ({s.name}) jumps between SLOW and FAST "
                                            "code; only calls and address loads may cross regions")
                    continue                        # same-region pc-relative: base independent
                else:
                    name = next((k for k, v in RV.items() if v == rtype), str(rtype))
                    unsupported.setdefault(name, []).append(f"{target.name}+{off:#x} ({s.name})")
        if unsupported:
            msg = "; ".join(f"{k} x{len(v)} e.g. {v[0]}" for k, v in unsupported.items())
            raise PackError(f"unsupported relocations: {msg}")

        if any(region(o) for o in abs_pcrel):
            raise PackError("FAST code references an absolute address with auipc (weak symbol?); "
                            "move that code out of PAPP_FAST")
        abs_fixups = _rewrite_abs_pcrel(image, abs_pcrel, lo12_sites, image_end)
        relocs = [(_tag(o, SITE_FAST), t, _tag(v, VAL_FAST)) for o, t, v in sorted(relocs)]
        # Label differences are base independent unless the two labels sit in
        # different regions: then the word moves by (FAST shift - SLOW shift).
        for off, (add, sub) in sorted(diffs.items()):
            if add is None or sub is None or region(add) == region(sub):
                continue
            if not stored(off):
                raise PackError(f"label difference at {off:#x} outside the stored image")
            relocs.append((_tag(off, SITE_FAST), R_DIFF32, 1 if region(add) else 0xFFFFFFFF))
        # Cross-region pairs: each PCREL_HI20 record, then the LO12 records that use it
        los = {}
        for o, t, label in lo12_sites:
            if label in cross:
                los.setdefault(label, []).append((o, t))
        for hi_off, tgt in sorted(cross.items()):
            relocs.append((_tag(hi_off, SITE_FAST), R_PCREL_HI20, _tag(tgt, VAL_FAST)))
            if hi_off not in los:
                raise PackError(f"auipc at {hi_off:#x} crossing regions has no matching low part")
            for o, t in los[hi_off]:
                kind = R_PCREL_LO12_S if t == RV["R_RISCV_PCREL_LO12_S"] else R_PCREL_LO12_I
                relocs.append((_tag(o, SITE_FAST), kind, _tag(hi_off, VAL_FAST)))
        imp_start, imp_end = sym("__papp_imports_start"), sym("__papp_imports_end")
        if (imp_end - imp_start) % IMPORT_SIZE:
            raise PackError("import table size is not a multiple of 12")
        manifest_off = syms["__papp_manifest"]["st_value"] if "__papp_manifest" in syms else 0
        for what, addr in (("_papp_start", sym("_papp_start")), ("import table", imp_start),
                           ("manifest", manifest_off)):
            if region(addr):
                raise PackError(f"{what} must not be in the FAST region")
        img = PappImage(image=image, bss_size=bss_end - image_end, entry_off=sym("_papp_start"),
                        imports_off=imp_start, imports_count=(imp_end - imp_start) // IMPORT_SIZE,
                        manifest_off=manifest_off, fast=fast, fast_bss_size=fast_bss_end - fast_end,
                        relocs=relocs, abs_fixups=abs_fixups)
        if manifest_off:
            m = image[manifest_off:manifest_off + 96]
            magic, sdk = struct.unpack_from("<II", m)
            if magic != MANIFEST_MAGIC:
                raise PackError("manifest magic mismatch")
            cstr = lambda b: b.split(b"\0")[0].decode("utf-8", "replace")
            img.manifest = dict(id=cstr(m[8:40]), name=cstr(m[40:72]), version=cstr(m[72:88]),
                                main_stack=struct.unpack_from("<I", m, 88)[0])
        return img


def relocate(img, base, fast_base=FAST_LINK_BASE):
    """Apply relocations exactly like the device loader (lx_app.c apply_relocs).
    Returns (SLOW image, FAST image)."""
    out = bytearray(img.image)
    fout = bytearray(img.fast)
    addr = lambda v: fast_base + (v & ~VAL_FAST) if v & VAL_FAST else base + v
    deltas = {}         # auipc site (tagged like a value) -> target - pc
    for site, typ, value in img.relocs:
        fast_site = bool(site & SITE_FAST)
        off = site & ~SITE_FAST
        buf = fout if fast_site else out
        insn = struct.unpack_from("<I", buf, off)[0]
        if typ == R_PCREL_HI20:
            pc = (fast_base if fast_site else base) + off
            v = deltas[off | (VAL_FAST if fast_site else 0)] = (addr(value) - pc) & 0xFFFFFFFF
            typ = R_HI20
        elif typ in (R_PCREL_LO12_I, R_PCREL_LO12_S):
            v = deltas[value]
            typ = R_LO12_I if typ == R_PCREL_LO12_I else R_LO12_S
        elif typ == R_DIFF32:
            shift = (fast_base - FAST_LINK_BASE) - base     # FAST moved by this much more than SLOW
            struct.pack_into("<I", buf, off, (insn + (shift if value == 1 else -shift)) & 0xFFFFFFFF)
            continue
        else:
            v = addr(value) & 0xFFFFFFFF
        lo = v & 0xFFF
        if typ == R_ABS32:
            insn = v
        elif typ == R_HI20:
            insn = (insn & 0xFFF) | ((v + 0x800) & 0xFFFFF000)
        elif typ == R_LO12_I:
            insn = (insn & 0x000FFFFF) | (lo << 20)
        elif typ == R_LO12_S:
            insn = (insn & 0x01FFF07F) | ((lo >> 5) << 25) | ((lo & 0x1F) << 7)
        struct.pack_into("<I", buf, off, insn & 0xFFFFFFFF)
    return out, fout


def serialize(img):
    if len(img.image) + img.bss_size >= SITE_FAST or len(img.fast) + img.fast_bss_size >= SITE_FAST:
        raise PackError("image too large for relocation encoding")
    relocs = b"".join(struct.pack("<II", off | (typ << 28), value) for off, typ, value in img.relocs)
    header = struct.pack("<IHHIIIIIIIIIIII8x",
                         MAGIC, FMT_VERSION, HEADER_SIZE, 0,
                         len(img.image), img.bss_size, img.entry_off,
                         img.imports_off, img.imports_count, img.manifest_off,
                         len(img.relocs), zlib.crc32(img.fast, zlib.crc32(img.image)) & 0xFFFFFFFF,
                         zlib.crc32(relocs) & 0xFFFFFFFF, len(img.fast), img.fast_bss_size)
    assert len(header) == HEADER_SIZE
    return header + bytes(img.image) + bytes(img.fast) + relocs


def main():
    ap = argparse.ArgumentParser(description="ELF → .papp v2")
    ap.add_argument("elf")
    ap.add_argument("out")
    args = ap.parse_args()
    try:
        img = build_image(args.elf)
        data = serialize(img)
    except PackError as e:
        print(f"papp_pack: error: {e}", file=sys.stderr)
        return 1
    with open(args.out, "wb") as f:
        f.write(data)
    kinds = {}
    for _, t, _ in img.relocs:
        kinds[t] = kinds.get(t, 0) + 1
    names = {R_ABS32: "abs32", R_HI20: "hi20", R_LO12_I: "lo12_i", R_LO12_S: "lo12_s",
             R_PCREL_HI20: "pcrel_hi20", R_PCREL_LO12_I: "pcrel_lo12_i", R_PCREL_LO12_S: "pcrel_lo12_s",
             R_DIFF32: "diff32"}
    m = img.manifest
    print(f"papp_pack: {args.out}: image {len(img.image):,} B, bss {img.bss_size:,} B, "
          f"relocs {len(img.relocs)} ({', '.join(f'{names[k]} {v}' for k, v in sorted(kinds.items()))}), "
          f"imports {img.imports_count}"
          + (f", FAST {len(img.fast):,} B + bss {img.fast_bss_size:,} B" if img.fast or img.fast_bss_size else "")
          + (f", app '{m['name']}' ({m['id']}) v{m['version']}" if m else ", no manifest"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
