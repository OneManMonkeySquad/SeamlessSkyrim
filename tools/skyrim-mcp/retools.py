"""Lightweight reverse-engineering helpers: Address Library ids, CommonLib names, disassembly.

Code is read from SkyrimSE.exe on disk (the original, unpatched by other mods' hooks); the
id <-> offset table comes from the running game once and is cached per runtime version.
"""

from __future__ import annotations

import bisect
import re
import struct
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_GRP_CALL, CS_GRP_JUMP, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

COMMONLIB = Path(__file__).resolve().parents[2] / "extern" / "CommonLibSSE-NG"
# Optional: Community Shaders hooks much of the renderer and names its hook targets.
COMMUNITY_SHADERS = Path(__file__).resolve().parents[2] / "reference" / "community-shaders"
# stl::detour_thunk<Main_RenderWorld>(REL::RelocationID(100424, 107142)) and similar.
CS_HOOK = re.compile(r"<(\w+)>\(\s*REL::RelocationID\(\s*(\d+)\s*,\s*(\d+)\s*\)(?!\.address)")


# ---- PE image on disk ---------------------------------------------------------------------


@dataclass
class Section:
    name: str
    rva: int
    vsize: int
    raw: int
    rawsize: int


class Image:
    def __init__(self, exe: Path) -> None:
        self.data = exe.read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        count = struct.unpack_from("<H", self.data, pe + 6)[0]
        optional_size = struct.unpack_from("<H", self.data, pe + 20)[0]
        table = pe + 24 + optional_size
        self.sections = []
        for i in range(count):
            base = table + i * 40
            name = self.data[base : base + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, rva, rawsize, raw = struct.unpack_from("<IIII", self.data, base + 8)
            self.sections.append(Section(name, rva, vsize, raw, rawsize))

    def section_of(self, rva: int) -> Section | None:
        for s in self.sections:
            if s.rva <= rva < s.rva + max(s.vsize, s.rawsize):
                return s
        return None

    def read(self, rva: int, size: int) -> bytes:
        s = self.section_of(rva)
        if s is None:
            raise ValueError(f"rva 0x{rva:X} is not inside any section")
        start = s.raw + (rva - s.rva)
        end = min(start + size, s.raw + s.rawsize)
        return self.data[start:end]

    def text(self) -> Section:
        return next(s for s in self.sections if s.name == ".text")


# ---- Address Library ids and CommonLib names ----------------------------------------------


class IdMap:
    def __init__(self, csv: Path) -> None:
        pairs = []
        for line in csv.read_text().splitlines()[1:]:
            id_text, off_text = line.split(",")
            pairs.append((int(off_text, 16), int(id_text)))
        pairs.sort()
        self.offsets = [p[0] for p in pairs]
        self.ids = [p[1] for p in pairs]
        self.by_id = {i: o for o, i in pairs}

    def offset_of(self, id_: int) -> int:
        return self.by_id[id_]

    def nearest(self, offset: int) -> tuple[int, int] | None:
        """(id, distance) of the closest id at or below offset."""
        i = bisect.bisect_right(self.offsets, offset) - 1
        if i < 0:
            return None
        return self.ids[i], offset - self.offsets[i]


FUNC_DEF = re.compile(r"^\s*(?:[\w:<>,\*&\s]+?\s)?([A-Za-z_]\w*(?:::[A-Za-z_~]\w*)+)\s*\([^;]*$")
RELOC = re.compile(r"(?:RELOCATION_ID|RelocationID|VariantID)\(\s*(\d+)\s*,\s*(\d+)")
TABLE = re.compile(r"(VTABLE|RTTI)_(\w+)\{(.*)\}")


def build_names() -> dict[int, str]:
    """AE id -> human name, harvested from CommonLib's sources."""
    names: dict[int, str] = {}
    for header in ("Offsets_VTABLE.h", "Offsets_RTTI.h"):
        for path in (COMMONLIB / "include" / "RE").glob(header):
            for line in path.read_text("utf-8", "replace").splitlines():
                m = TABLE.search(line)
                if not m:
                    continue
                kind, cls, body = m.groups()
                for n, (_, ae) in enumerate(RELOC.findall(body)):
                    if int(ae):
                        names.setdefault(int(ae), f"{kind.lower()} {cls}" + (f"[{n}]" if n else ""))
    for path in list((COMMONLIB / "src").rglob("*.cpp")) + list((COMMONLIB / "include").rglob("*.h")):
        if path.name.startswith("Offsets_"):
            continue
        current = None
        for line in path.read_text("utf-8", "replace").splitlines():
            if (m := FUNC_DEF.match(line)) and "RELOCATION_ID" not in line:
                current = m.group(1)
            for _, ae in RELOC.findall(line):
                if current and int(ae):
                    names.setdefault(int(ae), current)
    if COMMUNITY_SHADERS.exists():
        for path in (COMMUNITY_SHADERS / "src").rglob("*.*"):
            if path.suffix not in (".cpp", ".h"):
                continue
            for name, _, ae in CS_HOOK.findall(path.read_text("utf-8", "replace")):
                # CS names hooks Class_Method; mark them so they're not mistaken for CommonLib names.
                names.setdefault(int(ae), f"{name.replace('_', '::', 1)} [CS]")
    return names


# ---- disassembly --------------------------------------------------------------------------


class Disassembler:
    def __init__(self, image: Image, ids: IdMap, names: dict[int, str]) -> None:
        self.image, self.ids, self.names = image, ids, names
        self.cs = Cs(CS_ARCH_X86, CS_MODE_64)
        self.cs.detail = True

    def label(self, rva: int) -> str:
        hit = self.ids.nearest(rva)
        if not hit:
            return ""
        id_, delta = hit
        name = self.names.get(id_)
        base = f"ID {id_}" + (f" {name}" if name else "")
        # Far from any id means "somewhere inside a big function": show it, but it's a weak label.
        return base if delta == 0 else f"{base}+0x{delta:X}"

    def resolve(self, target: str) -> int:
        target = target.strip()
        if target.lower().startswith("id:"):
            return self.ids.offset_of(int(target[3:]))
        if target.lower().startswith("0x"):
            return int(target, 16)
        for id_, name in self.names.items():
            if name == target:
                return self.ids.offset_of(id_)
        raise ValueError(f"cannot resolve '{target}' (use id:<n>, 0x<offset> or a CommonLib name like TES::SetCurrentCell)")

    def disassemble(self, rva: int, max_instructions: int = 150, stop_at_end: bool = True) -> list[str]:
        code = self.image.read(rva, max_instructions * 15)
        lines = []
        for count, insn in enumerate(self.cs.disasm(code, rva)):
            notes = []
            if insn.group(CS_GRP_CALL) or insn.group(CS_GRP_JUMP):
                op = insn.operands[0]
                if op.type == X86_OP_IMM:
                    notes.append(self.label(op.imm))
            for op in insn.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    notes.append(self.label(insn.address + insn.size + op.mem.disp))
            note = "  ; " + " | ".join(n for n in notes if n) if any(notes) else ""
            lines.append(f"{insn.address:08X}  {insn.mnemonic:<6} {insn.op_str}{note}")
            # MSVC pads functions with int3; two in a row after the body means we've left it.
            if stop_at_end and insn.mnemonic == "int3" and count > 0 and lines[-2].split()[1] == "int3":
                lines.pop()
                lines.pop()
                break
            if count + 1 >= max_instructions:
                lines.append("... (max_instructions reached)")
                break
        return lines

    def xrefs(self, rva: int, limit: int = 100) -> list[str]:
        """Every instruction in .text whose rel32 operand resolves to rva: calls, jumps and
        RIP-relative lea/mov/cmp (e.g. code loading a vtable or a global)."""
        import numpy as np

        text = self.image.text()
        data = self.image.read(text.rva, text.rawsize)
        # rel32 at every byte offset i, assuming the displacement is the instruction's last field
        # (true for call/jmp/lea/mov with RIP-relative memory and no trailing immediate): a
        # reference to rva has rel32 == rva - (address after the field). Read as four interleaved
        # int32 views of the bytes rather than one int64 per byte offset, which took well over a
        # gigabyte beside a running game.
        found = []
        for phase in range(4):
            rel = np.frombuffer(data, dtype="<i4", count=(len(data) - phase) // 4, offset=phase)
            at = phase + 4 * np.arange(len(rel), dtype=np.int64)
            found.append(at[rel == rva - text.rva - 4 - at])
        hits = []
        for i in np.sort(np.concatenate(found))[: limit * 4]:
            site = text.rva + int(i)
            # Confirm by decoding from plausible instruction starts before the displacement; only
            # accept decodings that really use the rel32 as a branch target or RIP-relative operand
            # (otherwise e.g. "add eax, imm32" swallows the same bytes).
            for back in (3, 2, 4, 1, 5, 6):
                insn = next(self.cs.disasm(self.image.read(site - back, 16), site - back), None)
                if not insn or insn.address + insn.size != site + 4:
                    continue
                if "rip" in insn.op_str or insn.group(CS_GRP_CALL) or insn.group(CS_GRP_JUMP):
                    hits.append(f"{insn.address:08X}  {insn.mnemonic} {insn.op_str}  in {self.label(insn.address)}")
                    break
            if len(hits) >= limit:
                break
        return hits

    def callers(self, rva: int, limit: int = 100) -> list[str]:
        """Direct E8/E9 rel32 call/jmp sites targeting rva (misses indirect and vtable calls)."""
        text = self.image.text()
        data = self.image.read(text.rva, text.rawsize)
        hits = []
        for opcode in (0xE8, 0xE9):
            start = 0
            while len(hits) < limit:
                i = data.find(bytes([opcode]), start)
                if i < 0 or i + 5 > len(data):
                    break
                site = text.rva + i
                if site + 5 + struct.unpack_from("<i", data, i + 1)[0] == rva:
                    kind = "call" if opcode == 0xE8 else "jmp "
                    hits.append(f"{site:08X}  {kind} from {self.label(site)}")
                start = i + 1
        return hits


# ---- command line --------------------------------------------------------------------------
#
# server.py runs xrefs through this, in a process of its own with a time limit: inside the MCP
# server the scan hung three times out of sixteen (20 to 30 minutes each, until the user stepped
# in), with targets that take a second when run again, and nothing to show why.

if __name__ == "__main__":
    import json
    import sys

    exe, ids_csv, names_json, rva, limit = sys.argv[1:6]
    names = {int(k): v for k, v in json.loads(Path(names_json).read_text()).items()}
    print(json.dumps(Disassembler(Image(Path(exe)), IdMap(Path(ids_csv)), names).xrefs(int(rva), int(limit))))
