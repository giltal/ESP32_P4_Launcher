#!/usr/bin/env python3
"""
Host test for .papp v2 relocation (Spike A).

For each test app: build it linked at 0 (→ relocation table) and linked directly
at several real base addresses. Applying the relocation table to the base-0 image
must reproduce the directly-linked image byte for byte.

  python tests/host/test_reloc.py
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
import papp_build  # noqa: E402
import papp_pack   # noqa: E402

from elftools.elf.elffile import ELFFile  # noqa: E402

APPS = [
    ("hello", [os.path.join(ROOT, "apps", "hello", "main.c")], False),
    ("libc_test", [os.path.join(ROOT, "apps", "libc_test", "main.c")], True),
    ("fast_bench", [os.path.join(ROOT, "apps", "fast_bench", "main.c")], False),   # SLOW + FAST
]
# (SLOW base, FAST base): FAST below, just above and far above SLOW, like the
# internal SRAM / PSRAM addresses on the device
BASES = [(0x4A000000, 0x4FF20000), (0x48250000, 0x4FF4A340), (0x4BFF0000, 0x40000000)]


def flat_image(elf_path, base, fast_base):
    """(SLOW, FAST) images of an ELF linked directly at base / fast_base."""
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        syms = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
        slow = bytearray(syms["__papp_image_end"] - base)
        sec = elf.get_section_by_name(".fast")
        fast = bytearray(sec["sh_size"] if sec else 0)
        for sec in elf.iter_sections():
            if sec["sh_flags"] & 0x2 and sec["sh_type"] != "SHT_NOBITS" and sec["sh_size"]:
                a = sec["sh_addr"]
                buf, lo = (fast, a - fast_base) if sec.name == ".fast" else (slow, a - base)
                buf[lo:lo + sec["sh_size"]] = sec.data()
        return slow, fast


def main():
    failures = 0
    for name, srcs, libc in APPS:
        out = os.path.join(ROOT, "build", "test_reloc", name)
        elf0, _ = papp_build.build(name, srcs, libc=libc, out_dir=os.path.join(out, "base0"))
        img = papp_pack.build_image(elf0)
        for base, fast_base in BASES:
            elfb, _ = papp_build.build(name, srcs, libc=libc, out_dir=os.path.join(out, f"{base:08x}"),
                                       link_base=base, fast_link_base=fast_base)
            want, want_fast = flat_image(elfb, base, fast_base)
            got, got_fast = papp_pack.relocate(img, base, fast_base)
            # Sites the packer rewrote from auipc to lui (absolute targets) are encoded
            # differently from ld's output; check they still compute the same address.
            for hi_off, lo_offs, value in img.abs_fixups:
                hi = struct.unpack_from("<I", got, hi_off)[0] >> 12
                for lo_off in lo_offs:
                    insn = struct.unpack_from("<I", got, lo_off)[0]
                    imm = (insn >> 20) if (insn & 0x7F) != 0x23 else (((insn >> 25) << 5) | ((insn >> 7) & 0x1F))
                    imm -= (imm & 0x800) << 1
                    if ((hi << 12) + imm) & 0xFFFFFFFF != value:
                        print(f"FAIL {name}: rewritten site {hi_off:#x}/{lo_off:#x} != {value:#x}")
                        failures += 1
                    want[lo_off:lo_off + 4] = got[lo_off:lo_off + 4]
                want[hi_off:hi_off + 4] = got[hi_off:hi_off + 4]
            if want_fast != got_fast:
                diffs = [i for i in range(min(len(want_fast), len(got_fast))) if want_fast[i] != got_fast[i]]
                print(f"FAIL {name} FAST @ {fast_base:#010x}: sizes {len(want_fast)}/{len(got_fast)}, "
                      f"{len(diffs)} differing bytes, first at {diffs[0] if diffs else '-':#x}")
                failures += 1
            if want == got:
                print(f"PASS {name} @ {base:#010x} / FAST @ {fast_base:#010x}: {len(got):,} + "
                      f"{len(got_fast):,} bytes, {len(img.relocs)} relocs")
            else:
                diffs = [i for i in range(min(len(want), len(got))) if want[i] != got[i]]
                print(f"FAIL {name} @ {base:#010x}: sizes {len(want)}/{len(got)}, "
                      f"{len(diffs)} differing bytes, first at {diffs[0] if diffs else '-':#x}")
                failures += 1
    print("ALL PASSED" if not failures else f"{failures} FAILURE(S)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
