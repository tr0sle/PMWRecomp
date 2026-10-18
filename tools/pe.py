"""PE image loader + capstone decoder shared by the analysis and recompiler passes."""
import os, struct, pefile, capstone
from capstone import x86 as X

# folder with the installed PC game (PMR.exe); override with the PMWR_GAME_DIR environment variable
GAME_DIR = os.environ.get("PMWR_GAME_DIR", r"C:\Users\Anon\Desktop\PMWR")


class Image:
    def __init__(self, path=GAME_DIR + r"\PMR.exe"):
        pe = pefile.PE(path)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(self.size)
        hdr = pe.OPTIONAL_HEADER.SizeOfHeaders
        self.mem[:hdr] = pe.__data__[:hdr]
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            va = self.base + s.VirtualAddress
            raw = s.get_data()[: max(s.Misc_VirtualSize, s.SizeOfRawData)]
            raw = raw[: s.SizeOfRawData]
            self.mem[s.VirtualAddress : s.VirtualAddress + len(raw)] = raw
            self.sections.append((name, va, va + max(s.Misc_VirtualSize, s.SizeOfRawData), s.Characteristics))
        t = [s for s in self.sections if s[0] == ".text"][0]
        self.text_lo, self.text_hi = t[1], t[1] + pe.sections[0].Misc_VirtualSize
        # imports: IAT slot address -> "dll!name"
        self.imports = {}
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            for i in d.imports:
                nm = i.name.decode() if i.name else "#%d" % i.ordinal
                self.imports[i.address] = (d.dll.decode().lower(), nm)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self._cache = {}

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi

    def in_image(self, a):
        return self.base <= a < self.base + self.size

    def u8(self, a):
        return self.mem[a - self.base]

    def u32(self, a):
        o = a - self.base
        return struct.unpack_from("<I", self.mem, o)[0]

    def bytes(self, a, n):
        o = a - self.base
        return bytes(self.mem[o : o + n])

    def insn(self, a):
        r = self._cache.get(a)
        if r is None:
            r = next(self.md.disasm(self.bytes(a, 16), a, 1), None)
            self._cache[a] = r
        return r
