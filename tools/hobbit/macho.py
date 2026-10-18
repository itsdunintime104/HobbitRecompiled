"""Pure-Python parser for 32-bit little-endian Mach-O (MH_MAGIC 0xfeedface)."""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MH_MAGIC = 0xFEEDFACE

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_UNIXTHREAD = 0x5
LC_DYSYMTAB = 0xB
LC_LOAD_DYLIB = 0xC
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_REEXPORT_DYLIB = 0x8000001F
LC_LAZY_LOAD_DYLIB = 0x20
LC_LOAD_UPWARD_DYLIB = 0x80000023
LC_FUNCTION_STARTS = 0x26
LC_DATA_IN_CODE = 0x29

DYLIB_COMMANDS = {
    LC_LOAD_DYLIB,
    LC_LOAD_WEAK_DYLIB,
    LC_REEXPORT_DYLIB,
    LC_LAZY_LOAD_DYLIB,
    LC_LOAD_UPWARD_DYLIB,
}

S_NON_LAZY_SYMBOL_POINTERS = 0x6
S_LAZY_SYMBOL_POINTERS = 0x7
S_SYMBOL_STUBS = 0x8
S_MOD_INIT_FUNC_POINTERS = 0x9

INDIRECT_SYMBOL_LOCAL = 0x80000000
INDIRECT_SYMBOL_ABS = 0x40000000

N_STAB = 0xE0
N_TYPE = 0x0E
N_EXT = 0x01
N_UNDF = 0x0

# Index of eip in i386_thread_state_t:
# eax ebx ecx edx edi esi ebp esp ss eflags eip cs ds es fs gs
EIP_INDEX = 10


@dataclass
class Section:
    name: str
    addr: int
    size: int
    offset: int
    flags: int
    reserved1: int
    reserved2: int


@dataclass
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    initprot: int
    maxprot: int
    sections: list[Section] = field(default_factory=list)


@dataclass
class Symbol:
    name: str
    n_type: int
    n_sect: int
    n_desc: int
    n_value: int

    @property
    def is_undefined_external(self) -> bool:
        return (
            self.n_type & N_STAB == 0
            and self.n_type & N_TYPE == N_UNDF
            and self.n_type & N_EXT != 0
        )

    @property
    def library_ordinal(self) -> int:
        return (self.n_desc >> 8) & 0xFF


@dataclass
class DataInCode:
    addr: int
    length: int
    kind: int


@dataclass
class MachO:
    segments: list[Segment]
    dylibs: list[str]
    symbols: list[Symbol]
    indirect_symbols: list[int]
    function_starts: list[int]
    data_in_code: list[DataInCode]
    entry: int
    data: bytes

    def sections(self) -> list[tuple[Segment, Section]]:
        return [(seg, sec) for seg in self.segments for sec in seg.sections]

    def find_section(self, name: str) -> Section:
        for _, sec in self.sections():
            if sec.name == name:
                return sec
        raise KeyError(name)

    def segment_text(self) -> Segment:
        for seg in self.segments:
            if seg.name == "__TEXT":
                return seg
        raise KeyError("__TEXT")


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii")


def _read_uleb(data: bytes, pos: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7


def _parse_function_starts(data: bytes, base: int) -> list[int]:
    starts: list[int] = []
    addr = base
    pos = 0
    while pos < len(data):
        delta, pos = _read_uleb(data, pos)
        if delta == 0:
            break
        addr += delta
        starts.append(addr)
    return starts


def _parse_segment(data: bytes, off: int) -> Segment:
    name_raw, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, _flags = (
        struct.unpack_from("<16s8I", data, off + 8)
    )
    seg = Segment(
        _cstr(name_raw), vmaddr, vmsize, fileoff, filesize, initprot, maxprot
    )
    sec_off = off + 8 + 16 + 32
    for i in range(nsects):
        (sname, _segname, addr, size, offset, _align, _reloff, _nreloc,
         flags, res1, res2) = struct.unpack_from("<16s16s9I", data, sec_off + 68 * i)
        seg.sections.append(
            Section(_cstr(sname), addr, size, offset, flags, res1, res2)
        )
    return seg


def _parse_symbols(data: bytes, symoff: int, nsyms: int, stroff: int) -> list[Symbol]:
    symbols: list[Symbol] = []
    for i in range(nsyms):
        strx, n_type, n_sect, n_desc, n_value = struct.unpack_from(
            "<IBBHI", data, symoff + 12 * i
        )
        end = data.index(b"\0", stroff + strx)
        name = data[stroff + strx : end].decode("ascii", errors="replace")
        symbols.append(Symbol(name, n_type, n_sect, n_desc, n_value))
    return symbols


def parse(data: bytes) -> MachO:
    magic, _cpu, _sub, _ftype, ncmds, _sizeofcmds, _flags = struct.unpack_from(
        "<7I", data, 0
    )
    if magic != MH_MAGIC:
        raise ValueError(f"not a 32-bit little-endian Mach-O: magic {magic:#x}")

    segments: list[Segment] = []
    dylibs: list[str] = []
    symbols: list[Symbol] = []
    indirect_symbols: list[int] = []
    func_starts_loc: tuple[int, int] | None = None
    dic_loc: tuple[int, int] | None = None
    entry: int | None = None
    symtab: tuple[int, int, int] | None = None
    dysymtab: tuple[int, int] | None = None

    off = 28
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", data, off)
        if cmd == LC_SEGMENT:
            segments.append(_parse_segment(data, off))
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, _strsize = struct.unpack_from("<4I", data, off + 8)
            symtab = (symoff, nsyms, stroff)
        elif cmd == LC_DYSYMTAB:
            fields = struct.unpack_from("<18I", data, off + 8)
            dysymtab = (fields[12], fields[13])  # indirectsymoff, nindirectsyms
        elif cmd in DYLIB_COMMANDS:
            (name_off,) = struct.unpack_from("<I", data, off + 8)
            end = data.index(b"\0", off + name_off)
            dylibs.append(data[off + name_off : end].decode("utf-8"))
        elif cmd == LC_FUNCTION_STARTS:
            func_starts_loc = struct.unpack_from("<2I", data, off + 8)
        elif cmd == LC_DATA_IN_CODE:
            dic_loc = struct.unpack_from("<2I", data, off + 8)
        elif cmd == LC_UNIXTHREAD:
            _flavor, _count = struct.unpack_from("<2I", data, off + 8)
            state = struct.unpack_from("<16I", data, off + 16)
            entry = state[EIP_INDEX]
        off += cmdsize

    if entry is None:
        raise ValueError("no LC_UNIXTHREAD")
    if symtab is not None:
        symbols = _parse_symbols(data, *symtab[:2], symtab[2])
    if dysymtab is not None:
        indoff, nind = dysymtab
        indirect_symbols = list(struct.unpack_from(f"<{nind}I", data, indoff))

    text_seg = next(s for s in segments if s.name == "__TEXT")
    function_starts: list[int] = []
    if func_starts_loc is not None:
        dataoff, datasize = func_starts_loc
        function_starts = _parse_function_starts(
            data[dataoff : dataoff + datasize], text_seg.vmaddr
        )

    data_in_code: list[DataInCode] = []
    if dic_loc is not None:
        dataoff, datasize = dic_loc
        for i in range(datasize // 8):
            offset, length, kind = struct.unpack_from("<IHH", data, dataoff + 8 * i)
            data_in_code.append(DataInCode(offset, length, kind))

    return MachO(
        segments, dylibs, symbols, indirect_symbols,
        function_starts, data_in_code, entry, data,
    )
