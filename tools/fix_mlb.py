#!/usr/bin/env python3
"""Rewrite llvm-mos's .mlb label file so Mesen can actually use it.

    python3 tools/fix_mlb.py builds/cmake-build-nes-release/demo

Takes the stem of the built ROM and expects <stem>.nes, <stem>.nes.elf and
<stem>.mlb beside each other. Rewrites <stem>.mlb in place.

WHY
    The llvm-mos SDK emits the .mlb itself (it shows up even for CMake's
    compiler-id probe), and its `G:` code addresses are raw ELF VMAs: `e000`
    for fixed-bank code, and the bank-encoded pseudo-VMAs `18000`, `5826c`,
    `8a400` for banked sections. Mesen wants PRG FILE OFFSETS -- which is what
    its profiler reports as `PRG: $xxxxx`. So without this, Mesen names none of
    the hot functions and every capture has to be resolved by hand afterwards.

    The prefix is wrong too, not just the address. Per Mesen's integration docs
    the label types are `P:` PRG ROM, `R:` internal RAM, `S:` save RAM,
    `W:` work RAM and `G:` REGISTERS (PPU/APU/mapper). The SDK marks code `G:`,
    so Mesen reads every function as a register definition and shows bare
    addresses in the profiler and debugger. Converted code labels are therefore
    re-typed `P:` as well as re-addressed.

    `W:` and `R:` entries are already correct and are passed through untouched.
    So are `G:` entries that resolve to no ELF section -- for a genuine hardware
    register (PPUCTRL and friends) `G:` is the right type.

THE MAPPING
    demo/link.ld documents the encoding: a banked domain's ORIGIN is
    (bank + 1) << 16 | cpu_address, because a 16-bit JSR operand truncates it
    back to a plain $8000/$A000 call. So:

        vma >= 0x10000:  bank = (vma >> 16) - 1
                         prg  = bank * 0x2000 + (vma & 0x1FFF)
        otherwise:       the fixed 16 KiB at $C000-$FFFF, last in the ROM
                         prg  = vma + 0x30000

    This is NOT taken on trust. link.ld's own comment warns that the ORIGIN
    encoding and the FULL() order "are kept in agreement by hand", so a domain
    could be encoded as bank N while physically sitting somewhere else. Every
    rewritten address is therefore VERIFIED by comparing the ROM bytes at the
    computed PRG offset against the ELF bytes at that VMA, and a mismatch fails
    the build instead of producing a label file that quietly points at the wrong
    code. That check is the whole reason this is safe to wire into the build.

IDEMPOTENT
    Safe to run twice: it detects an already-rewritten file up front and exits
    without touching it. (The build regenerates the SDK's .mlb on every link, so
    the normal path always sees fresh input.)
"""
import struct
import sys
from pathlib import Path

INES_HEADER = 16          # iNES header, not part of the PRG image Mesen indexes
BANK        = 0x2000      # MMC3 PRG bank
FIXED_BIAS  = 0x30000     # $C000 -> PRG 0x3C000: the fixed 16 KiB is last
VERIFY_LEN  = 8           # bytes compared per symbol; enough to catch a misbank


def elf_sections(path):
    """[(vma, size, file_offset, name)] for SHF_ALLOC sections with contents.

    Hand-rolled rather than shelling out to llvm-readelf: this runs on every
    link, and the ELF32 section header table is a dozen lines to walk.
    """
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        sys.exit(f"fix_mlb: {path} is not an ELF32 file")
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)

    def hdr(i):
        off = e_shoff + i * e_shentsize
        # name, type, flags, addr, offset, size
        return struct.unpack_from("<IIIIII", data, off)

    _, _, _, _, str_off, _ = hdr(e_shstrndx)
    out = []
    for i in range(e_shnum):
        nm, typ, flags, addr, off, size = hdr(i)
        # SHF_ALLOC only. Without the flags test, .debug_* sections match by
        # address and every symbol "resolves" into debug data.
        if typ == 8 or not size or not flags & 0x2:
            continue
        end = data.index(b"\0", str_off + nm)
        out.append((addr, size, off, data[str_off + nm:end].decode()))
    return data, out


def to_prg(vma):
    if vma >= 0x10000:
        bank = (vma >> 16) - 1
        return bank * BANK + (vma & (BANK - 1))
    return vma + FIXED_BIAS


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    stem = Path(sys.argv[1])
    elf, mlb, nes = Path(f"{stem}.nes.elf"), Path(f"{stem}.mlb"), Path(f"{stem}.nes")
    for p in (elf, mlb, nes):
        if not p.is_file():
            sys.exit(f"fix_mlb: missing {p}")

    elf_bytes, sections = elf_sections(elf)
    # PRG only -- CHR follows it in the file and Mesen indexes the two
    # separately, so slicing at the header alone would make every offset near
    # the end of PRG "verify" against pattern data.
    rom = nes.read_bytes()
    prg = rom[INES_HEADER:INES_HEADER + rom[4] * 16384]

    def section_of(vma):
        for addr, size, off, name in sections:
            if addr <= vma < addr + size:
                return addr, size, off, name
        return None

    lines = mlb.read_text().splitlines()

    # Decide ONCE for the whole file whether this is fresh SDK output, rather
    # than per line. Fresh output has essentially every `G:` address inside an
    # ELF VMA section; a file this script has already rewritten has almost
    # none. Judging line by line looked fine but silently re-converted the
    # handful of PRG offsets that happen to land inside some section's VMA
    # range -- and the byte check cannot catch that, because a double-converted
    # address can still point at bytes that match.
    code = [l.strip() for l in lines
            if l.strip().startswith("G:") and l.count(":") >= 2]
    resolvable = 0
    for l in code:
        try:
            resolvable += section_of(int(l.split(":")[1].split("-")[0], 16)) is not None
        except ValueError:
            pass
    # Re-typing to P: makes this stronger than it was: a rewritten file has no
    # G: code lines left to resolve at all, so `code` shrinks to the handful of
    # genuine registers and unplaceable ca65 symbols.
    if code and resolvable * 2 < len(code):
        print(f"fix_mlb: {mlb.name} already converted "
              f"({resolvable}/{len(code)} G: lines still look like VMAs) -- nothing to do")
        return

    out, converted, passed, dropped, failures = [], 0, 0, 0, []
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith(";") or line.count(":") < 2:
            out.append(raw)
            continue
        kind, addrs, rest = line.split(":", 2)
        if kind != "G":                 # W:/R:/P: are already correct
            out.append(raw)
            continue

        # Addresses may be a single value or a `start-end` range.
        try:
            parts = [int(a, 16) for a in addrs.split("-")]
        except ValueError:
            out.append(raw)
            continue

        # No containing section means one of two things, and both are
        # pass-through. Either the address is already a PRG offset (this script
        # ran before -- that is what makes it idempotent), or the symbol came
        # from a ca65 object and carries a raw $8000-window address with no bank
        # encoding at all, so the ELF cannot say which bank it is in. The
        # FamiStudio symbols are the latter: `famistudio_update` at $8182 could
        # be any banked domain. Leaving them alone keeps them exactly as broken
        # as they are today rather than converting them to a confidently wrong
        # offset -- see 99-REFERENCE-address-mapping.md.
        sec = section_of(parts[0])
        if sec is None:
            out.append(raw)
            passed += 1
            continue

        # A `G:` label that does not land inside the PRG image is not ROM code
        # at all and Mesen has nothing to point it at. The log metadata is the
        # real case: `.pnes_log` sits at the sentinel VMA 0x04000000 because
        # log()'s strings are extracted at build time (see lua_log_builder) and
        # never ship. Drop those rather than emit a label aimed at nothing.
        if not all(0 <= to_prg(v) < len(prg) for v in parts):
            dropped += 1
            continue

        new = []
        for vma in parts:
            p = to_prg(vma)
            addr, size, off, name = section_of(vma)
            n = min(VERIFY_LEN, size - (vma - addr), len(prg) - p)
            want = elf_bytes[off + (vma - addr): off + (vma - addr) + n]
            if n > 0 and prg[p:p + n] != want:
                failures.append(
                    f"{rest}: VMA {vma:#x} ({name}) -> PRG {p:#x} but the ROM "
                    f"bytes there are {prg[p:p + n].hex()}, not {want.hex()}")
                break
            new.append(f"{p:x}")
        else:
            # P:, not G: -- this is PRG ROM code, not a register.
            out.append(f"P:{'-'.join(new)}:{rest}")
            converted += 1
            continue
        out.append(raw)

    if failures:
        print(f"fix_mlb: {len(failures)} address(es) failed verification -- the "
              f"bank encoding in demo/link.ld no longer matches the ROM layout.",
              file=sys.stderr)
        print("fix_mlb: link.ld warns these are kept in agreement BY HAND; the "
              "FULL() order, the ORIGINs and banks.hpp's section() must all "
              "agree. Fix those rather than this script.", file=sys.stderr)
        for f in failures[:10]:
            print(f"  {f}", file=sys.stderr)
        if len(failures) > 10:
            print(f"  ... and {len(failures) - 10} more", file=sys.stderr)
        sys.exit(1)

    mlb.write_text("\n".join(out) + "\n")
    note = f", {passed} left alone (already offsets, or ca65 with no bank)" if passed else ""
    note += f", {dropped} dropped as not ROM-resident" if dropped else ""
    print(f"fix_mlb: {converted} code label(s) rewritten to PRG offsets and "
          f"byte-verified against the ROM{note}")


if __name__ == "__main__":
    main()
