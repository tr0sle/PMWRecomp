"""x86-32 -> C static recompiler code generator for PMR.exe."""
import os, sys, collections
from analyze import *
from capstone import x86 as X, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET

R32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
SUBREGS = {
    "ax": ("eax", 0, 16), "cx": ("ecx", 0, 16), "dx": ("edx", 0, 16), "bx": ("ebx", 0, 16),
    "sp": ("esp", 0, 16), "bp": ("ebp", 0, 16), "si": ("esi", 0, 16), "di": ("edi", 0, 16),
    "al": ("eax", 0, 8), "cl": ("ecx", 0, 8), "dl": ("edx", 0, 8), "bl": ("ebx", 0, 8),
    "ah": ("eax", 8, 8), "ch": ("ecx", 8, 8), "dh": ("edx", 8, 8), "bh": ("ebx", 8, 8),
}
MASK = {1: "0xffu", 2: "0xffffu", 4: "0xffffffffu"}
STY = {1: "int8_t", 2: "int16_t", 4: "int32_t"}
BITS = {1: 8, 2: 16, 4: 32}
CCS = ("o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g")
CC_ALIAS = {"jz": "je", "jnz": "jne", "jc": "jb", "jnae": "jb", "jnb": "jae", "jnc": "jae", "jna": "jbe",
            "jnbe": "ja", "jpe": "jp", "jpo": "jnp", "jnge": "jl", "jnl": "jge", "jng": "jle", "jnle": "jg"}
CC_FLAGS = {"o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf", "be": "(cf|zf)",
            "a": "!(cf|zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf", "l": "(sf!=of)",
            "ge": "(sf==of)", "le": "(zf|(sf!=of))", "g": "(!zf&&(sf==of))"}


def hexs(v):
    return "0x%xu" % (v & 0xFFFFFFFF)


def imp_name(dll, name):
    dll = dll.lower().replace(".dll", "").replace(".", "_")
    return "hle_%s_%s" % (dll, name.replace("@", "_").replace("?", "_"))


class FuncGen:
    def __init__(self, P, f):
        self.P = P
        self.f = f
        self.img = P.img
        self.out = []
        fp = f["fpu"]
        self.static = fp[0] == "static"
        self.dmap = fp[1] if self.static else None
        self.d = 0
        self.uses_fpu = False

    # ------------------------------------------------------------ operands
    def reg(self, i, rid):
        return i.reg_name(rid)

    def rreg(self, name):
        if name in R32:
            return name
        b, sh, bits = SUBREGS[name]
        m = (1 << bits) - 1
        return "((%s>>%d)&0x%xu)" % (b, sh, m) if sh else "(%s&0x%xu)" % (b, m)

    def wreg(self, name, e):
        if name in R32:
            return "%s=%s;" % (name, e)
        b, sh, bits = SUBREGS[name]
        m = ((1 << bits) - 1) << sh
        return "%s=(%s&0x%xu)|(((uint32_t)(%s)&0x%xu)<<%d);" % (b, b, (~m) & 0xFFFFFFFF, e, (1 << bits) - 1, sh)

    def addr(self, i, op, seg=True):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg(i, m.base))
        if m.index:
            ix = self.reg(i, m.index)
            parts.append("%s*%d" % (ix, m.scale) if m.scale != 1 else ix)
        if m.disp or not parts:
            parts.append(hexs(m.disp))
        e = "+".join(parts)
        if seg and m.segment and self.reg(i, m.segment) == "fs":
            e = "FS_BASE+" + e
        return "(uint32_t)(%s)" % e

    def ld(self, a, sz):
        return {1: "LD8(%s)", 2: "LD16(%s)", 4: "LD32(%s)"}[sz] % a

    def st(self, a, sz, v):
        return {1: "ST8(%s,%s);", 2: "ST16(%s,%s);", 4: "ST32(%s,%s);"}[sz] % (a, v)

    def rd(self, i, op, sz=None):
        sz = sz or op.size
        if op.type == X.X86_OP_REG:
            n = self.reg(i, op.reg)
            if n in R32 or n in SUBREGS:
                return self.rreg(n)
            raise Unsupported("reg " + n)
        if op.type == X.X86_OP_IMM:
            return hexs(op.imm & int(MASK[sz][:-1], 16))
        if op.type == X.X86_OP_MEM:
            return self.ld(self.addr(i, op), sz)
        raise Unsupported("op")

    def wr(self, i, op, e):
        if op.type == X.X86_OP_REG:
            n = self.reg(i, op.reg)
            if n in R32 or n in SUBREGS:
                return self.wreg(n, e)
            raise Unsupported("wreg " + n)
        if op.type == X.X86_OP_MEM:
            return self.st(self.addr(i, op), op.size, e)
        raise Unsupported("wr")

    # ------------------------------------------------------------ flags
    def flags_from(self, kind, sz, need):
        """emit flag computations from ta,tb,tr (masked) for 'need' flags"""
        b = BITS[sz]
        out = []
        if need & ZF:
            out.append("zf=(tr==0);")
        if need & SF:
            out.append("sf=(tr>>%d)&1;" % (b - 1))
        if need & PF:
            out.append("pf=PARITY(tr);")
        if kind in ("sub", "cmp"):
            if need & CF:
                out.append("cf=(ta<tb);")
            if need & OF:
                out.append("of=(((ta^tb)&(ta^tr))>>%d)&1;" % (b - 1))
        elif kind == "add":
            if need & CF:
                out.append("cf=(tr<ta);")
            if need & OF:
                out.append("of=((~(ta^tb)&(ta^tr))>>%d)&1;" % (b - 1))
        elif kind == "logic":
            if need & CF:
                out.append("cf=0;")
            if need & OF:
                out.append("of=0;")
        elif kind == "inc":
            if need & OF:
                out.append("of=(tr==%s);" % hexs(1 << (b - 1)))
        elif kind == "dec":
            if need & OF:
                out.append("of=(ta==%s);" % hexs(1 << (b - 1)))
        elif kind == "neg":
            if need & CF:
                out.append("cf=(ta!=0);")
            if need & OF:
                out.append("of=(ta==%s);" % hexs(1 << (b - 1)))
        return "".join(out)

    def fused_cond(self, kind, sz, cc):
        S = STY[sz]
        if kind == "sub":
            t = {"b": "ta<tb", "ae": "ta>=tb", "e": "ta==tb", "ne": "ta!=tb", "be": "ta<=tb", "a": "ta>tb",
                 "l": "(%s)ta<(%s)tb" % (S, S), "ge": "(%s)ta>=(%s)tb" % (S, S),
                 "le": "(%s)ta<=(%s)tb" % (S, S), "g": "(%s)ta>(%s)tb" % (S, S),
                 "s": "(%s)tr<0" % S, "ns": "(%s)tr>=0" % S}
        else:
            t = {"e": "tr==0", "ne": "tr!=0", "s": "(%s)tr<0" % S, "ns": "(%s)tr>=0" % S,
                 "l": "(%s)tr<0" % S, "ge": "(%s)tr>=0" % S, "le": "(%s)tr<=0" % S, "g": "(%s)tr>0" % S,
                 "b": "0", "ae": "1", "be": "tr==0", "a": "tr!=0", "o": "0", "no": "1"}
        return t.get(cc)

    def cond(self, cc):
        if self.fused is not None:
            kind, sz = self.fused
            c = self.fused_cond(kind, sz, cc)
            assert c is not None
            return "(" + c + ")"
        return CC_FLAGS[cc]

    # ------------------------------------------------------------ x87
    def S(self, n):
        self.uses_fpu = True
        if self.static:
            k = self.d - 1 - n
            if k < 0:
                raise Unsupported("x87 underflow")
            return "f%d" % k
        return "ST(%d)" % n

    def fpush(self, e):
        self.uses_fpu = True
        if self.static:
            s = "f%d=%s;" % (self.d, e)
            self.d += 1
            return s
        return "FPUSH(%s);" % e

    def fpop(self):
        if self.static:
            self.d -= 1
            return ""
        return "FPOP();"

    def spill(self):
        if not self.static:
            return ""
        return "".join("FPUSH(f%d);" % k for k in range(self.d))

    def reload(self, n):
        if not self.static:
            return ""
        self.uses_fpu = self.uses_fpu or n > 0
        return "".join("f%d=ST(0);FPOP();" % k for k in reversed(range(n)))

    def x87(self, a, i):
        b = i.bytes
        k = 0
        while b[k] in (0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0xF0):
            k += 1
        op = b[k]
        if op == 0x9B:
            return ""
        modrm = b[k + 1]
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        S = self.S
        if mod != 3:
            memop = [o for o in i.operands if o.type == X.X86_OP_MEM][0]
            A = self.addr(i, memop)
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                src = {0xD8: "LDF32(%s)", 0xDC: "LDF64(%s)", 0xDA: "(double)(int32_t)LD32(%s)",
                       0xDE: "(double)(int16_t)LD16(%s)"}[op] % A
                s0 = S(0)
                if reg == 0: return "%s=%s+%s;" % (s0, s0, src)
                if reg == 1: return "%s=%s*%s;" % (s0, s0, src)
                if reg == 2: return "fsw=fcmp_sw(fsw,%s,%s);" % (s0, src)
                if reg == 3: return "fsw=fcmp_sw(fsw,%s,%s);" % (s0, src) + self.fpop()
                if reg == 4: return "%s=%s-%s;" % (s0, s0, src)
                if reg == 5: return "%s=%s-%s;" % (s0, src, s0)
                if reg == 6: return "%s=%s/%s;" % (s0, s0, src)
                if reg == 7: return "%s=%s/%s;" % (s0, src, s0)
            if op == 0xD9:
                if reg == 0: return self.fpush("LDF32(%s)" % A)
                if reg == 2: return "STF32(%s,%s);" % (A, S(0))
                if reg == 3: return "STF32(%s,%s);" % (A, S(0)) + self.fpop()
                if reg == 5: return "g_cpu.fcw=LD16(%s);" % A
                if reg == 7: return "ST16(%s,g_cpu.fcw);" % A
                if reg == 4: return "/*fldenv*/"
                if reg == 6: return "memset(GPTR(%s),0,28);ST16(%s,g_cpu.fcw);ST16(%s+4,fsw);" % (A, A, A)
            if op == 0xDB:
                if reg == 0: return self.fpush("(double)(int32_t)LD32(%s)" % A)
                if reg == 1: return "ST32(%s,(uint32_t)ftrunc64(%s));" % (A, S(0)) + self.fpop()
                if reg == 2: return "ST32(%s,fist32(%s));" % (A, S(0))
                if reg == 3: return "ST32(%s,fist32(%s));" % (A, S(0)) + self.fpop()
                if reg == 5: return self.fpush("ld80(%s)" % A)
                if reg == 7: return "st80(%s,%s);" % (A, S(0)) + self.fpop()
            if op == 0xDD:
                if reg == 0: return self.fpush("LDF64(%s)" % A)
                if reg == 1: return "ST64(%s,ftrunc64(%s));" % (A, S(0)) + self.fpop()
                if reg == 2: return "STF64(%s,%s);" % (A, S(0))
                if reg == 3: return "STF64(%s,%s);" % (A, S(0)) + self.fpop()
                if reg == 7: return "ST16(%s,fsw);" % A
            if op == 0xDF:
                if reg == 0: return self.fpush("(double)(int16_t)LD16(%s)" % A)
                if reg == 1: return "ST16(%s,(uint32_t)ftrunc64(%s));" % (A, S(0)) + self.fpop()
                if reg == 2: return "ST16(%s,fist16(%s));" % (A, S(0))
                if reg == 3: return "ST16(%s,fist16(%s));" % (A, S(0)) + self.fpop()
                if reg == 5: return self.fpush("(double)(int64_t)LD64(%s)" % A)
                if reg == 7: return "ST64(%s,fist64(%s));" % (A, S(0)) + self.fpop()
            raise Unsupported("x87 mem %02x /%d" % (op, reg))
        x = modrm
        if op == 0xD8:
            s0, si = S(0), S(rm)
            return {0: "%s=%s+%s;" % (s0, s0, si), 1: "%s=%s*%s;" % (s0, s0, si),
                    2: "fsw=fcmp_sw(fsw,%s,%s);" % (s0, si), 3: None,
                    4: "%s=%s-%s;" % (s0, s0, si), 5: "%s=%s-%s;" % (s0, si, s0),
                    6: "%s=%s/%s;" % (s0, s0, si), 7: "%s=%s/%s;" % (s0, si, s0)}[reg] or \
                ("fsw=fcmp_sw(fsw,%s,%s);" % (s0, si) + self.fpop())
        if op == 0xD9:
            if reg == 0:
                return self.fpush(S(rm))
            if reg == 1:
                s0, si = S(0), S(rm)
                return "{double _t=%s;%s=%s;%s=_t;}" % (s0, s0, si, si)
            if x == 0xD0: return ""
            s0 = S(0) if x not in range(0xE8, 0xEF) else None
            if x == 0xE0: return "%s=-%s;" % (s0, s0)
            if x == 0xE1: return "%s=fabs(%s);" % (s0, s0)
            if x == 0xE4: return "fsw=fcmp_sw(fsw,%s,0.0);" % s0
            if x == 0xE5: return "fsw=fxam_sw(fsw,%s,0);" % s0
            consts = {0xE8: "1.0", 0xE9: "3.321928094887362347870319429489390175864831393", 0xEA: "1.442695040888963407359924681001892137426645954",
                      0xEB: "3.141592653589793238462643383279502884197169399", 0xEC: "0.301029995663981195213738894724493026768189881",
                      0xED: "0.693147180559945309417232121458176568075500134", 0xEE: "0.0"}
            if x in consts: return self.fpush(consts[x])
            if x == 0xF0: return "%s=exp2(%s)-1.0;" % (s0, s0)
            if x == 0xF1:
                s1 = S(1)
                return "%s=%s*log2(%s);" % (s1, s1, s0) + self.fpop()
            if x == 0xF2: return "%s=tan(%s);" % (s0, s0) + self.fpush("1.0")
            if x == 0xF3:
                s1 = S(1)
                return "%s=atan2(%s,%s);" % (s1, s1, s0) + self.fpop()
            if x == 0xF4:
                return "{int _e;double _m=frexp(%s,&_e);%s=(double)(_e-1);%s}" % (s0, s0, self.fpush("_m*2.0"))
            if x == 0xF5: return "%s=remainder(%s,%s);fsw&=~0x400u;" % (s0, s0, S(1))
            if x == 0xF8: return "%s=fmod(%s,%s);fsw&=~0x400u;" % (s0, s0, S(1))
            if x == 0xF9:
                s1 = S(1)
                return "%s=%s*log2(%s+1.0);" % (s1, s1, s0) + self.fpop()
            if x == 0xFA: return "%s=sqrt(%s);" % (s0, s0)
            if x == 0xFB: return "{double _t=%s;%s=sin(_t);%s}" % (s0, s0, self.fpush("cos(_t)"))
            if x == 0xFC: return "%s=fround(%s);" % (s0, s0)
            if x == 0xFD: return "%s=ldexp(%s,(int)trunc(%s));" % (s0, s0, S(1))
            if x == 0xFE: return "%s=sin(%s);" % (s0, s0)
            if x == 0xFF: return "%s=cos(%s);" % (s0, s0)
            raise Unsupported("x87 d9 %02x" % x)
        if op == 0xDA and x == 0xE9:
            r = "fsw=fcmp_sw(fsw,%s,%s);" % (S(0), S(1))
            return r + self.fpop() + self.fpop()
        if op == 0xDB:
            if x == 0xE2: return "fsw&=0x7f00u;"
            if x == 0xE3: return "g_cpu.fcw=0x37f;fsw=0;"
            raise Unsupported("x87 db %02x" % x)
        if op == 0xDC:
            s0, si = S(0), S(rm)
            return {0: "%s=%s+%s;" % (si, si, s0), 1: "%s=%s*%s;" % (si, si, s0),
                    2: "fsw=fcmp_sw(fsw,%s,%s);" % (s0, si), 3: "fsw=fcmp_sw(fsw,%s,%s);" % (s0, si),
                    4: "%s=%s-%s;" % (si, s0, si), 5: "%s=%s-%s;" % (si, si, s0),
                    6: "%s=%s/%s;" % (si, s0, si), 7: "%s=%s/%s;" % (si, si, s0)}[reg] + \
                (self.fpop() if reg == 3 else "")
        if op == 0xDD:
            if reg == 0: return ""
            if reg in (2, 3):
                r = "%s=%s;" % (S(rm), S(0)) if rm else ""
                return r + (self.fpop() if reg == 3 else "")
            if reg in (4, 5):
                return "fsw=fcmp_sw(fsw,%s,%s);" % (S(0), S(rm)) + (self.fpop() if reg == 5 else "")
            raise Unsupported("x87 dd")
        if op == 0xDE:
            if x == 0xD9:
                r = "fsw=fcmp_sw(fsw,%s,%s);" % (S(0), S(1))
                return r + self.fpop() + self.fpop()
            s0, si = S(0), S(rm)
            r = {0: "%s=%s+%s;" % (si, si, s0), 1: "%s=%s*%s;" % (si, si, s0),
                 4: "%s=%s-%s;" % (si, s0, si), 5: "%s=%s-%s;" % (si, si, s0),
                 6: "%s=%s/%s;" % (si, s0, si), 7: "%s=%s/%s;" % (si, si, s0)}.get(reg)
            if r is None:
                raise Unsupported("x87 de")
            return r + self.fpop()
        if op == 0xDF:
            if x == 0xE0: return "eax=(eax&0xffff0000u)|(fsw&0xffffu);"
            if reg in (5, 6):  # fucomip / fcomip
                s0, si = S(0), S(rm)
                return ("{double _a=%s,_b=%s;zf=(_a==_b)||(_a!=_a)||(_b!=_b);pf=(_a!=_a)||(_b!=_b);cf=(_a<_b)||pf;of=0;sf=0;}" % (s0, si)) + self.fpop()
            raise Unsupported("x87 df %02x" % x)
        raise Unsupported("x87 %02x %02x" % (op, x))

    # ------------------------------------------------------------ calls
    def call_target_code(self, i, op):
        """returns C code performing the call (after return address pushed)"""
        img = self.img
        if op.type == X.X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t in self.P.funcs:
                return "F_%06x();" % t
            return "CALLI(%s);" % hexs(t)
        mt = mem_target(op)
        if mt is not None and mt in img.imports:
            dll, nm = img.imports[mt]
            self.P.used_imports.add(mt)
            return "%s();" % imp_name(dll, nm)
        return "CALLI(%s);" % self.rd(i, op, 4)

    def emit_call(self, a, i):
        op = i.operands[0]
        nxt = a + i.size
        t = op.imm & 0xFFFFFFFF if op.type == X.X86_OP_IMM else None
        if t in INTRINSIC_FPU:
            s0 = self.S(0)
            r = "{uint64_t _v=ftrunc64(%s);eax=(uint32_t)_v;edx=(uint32_t)(_v>>32);}" % s0
            return r + self.fpop()
        # compute target expression before pushing (operand may reference esp)
        pre = ""
        if op.type != X.X86_OP_IMM:
            mt = mem_target(op)
            if not (mt is not None and mt in self.img.imports):
                pre = "{uint32_t _t=%s;" % self.rd(i, op, 4)
                code = "CALLI(_t);"
            else:
                code = self.call_target_code(i, op)
        else:
            code = self.call_target_code(i, op)
        d_before = self.d
        s = pre + self.spill() + "esp-=4;ST32(esp,%s);SYNC_OUT;" % hexs(nxt) + code + "SYNC_IN;"
        if self.static:
            d_after = self.dmap.get(nxt)
            if d_after is None:
                d_after = d_before
            self.d = 0
            s += self.reload(d_after)
            self.d = d_after
        if pre:
            s += "}"
        return s

    def tail_code(self, t_code):
        return self.spill() + "SYNC_OUT;" + t_code + "return;"

    def jump_to(self, t):
        """C statement for control transfer to address t (goto or tail call)."""
        if t in self.f["insns"] and t not in self.f["tails"]:
            self.labels_used.add(t)
            return "goto L_%06x;" % t
        if t in self.P.funcs:
            return self.tail_code("F_%06x();" % t)
        return self.tail_code("CALLI(%s);" % hexs(t))

    # ------------------------------------------------------------ main
    def gen_insn(self, a, i):
        m = i.mnemonic
        self._cur_opstr = i.op_str
        ops = i.operands
        if self.static:
            self.d = self.dmap.get(a, 0)
        if i.group(CS_GRP_RET):
            n = ops[0].imm if ops else 0
            return "esp+=%d;" % (4 + n) + self.spill() + "SYNC_OUT;return;"
        if fpu_effect(i) is not None:
            return self.x87(a, i)
        if m in CC_ALIAS:
            m = CC_ALIAS[m]
        if m[0] == "j" and m[1:] in CCS:
            t = ops[0].imm & 0xFFFFFFFF
            return "if(%s){%s}" % (self.cond(m[1:]), self.jump_to(t))
        if m == "jecxz":
            return "if(!ecx){%s}" % self.jump_to(ops[0].imm & 0xFFFFFFFF)
        if m == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                return self.jump_to(op.imm & 0xFFFFFFFF)
            if a in self.f["jt"]:
                T, targets = self.f["jt"][a]
                ix = self.reg(i, op.mem.index)
                cases = "".join("case %s:%s" % (hexs(t), self.jump_to(t)) for t in sorted(set(targets)))
                return "switch(LD32(%s+%s*4)){%sdefault:bad_jump(%s,LD32(%s+%s*4));}" % (
                    hexs(op.mem.disp), ix, cases, hexs(a), hexs(op.mem.disp), ix)
            mt = mem_target(op)
            if mt is not None and mt in self.img.imports:
                return self.tail_code(self.call_target_code(i, op))
            return "{uint32_t _t=%s;%s}" % (self.rd(i, op, 4), self.tail_code("CALLI(_t);"))
        if i.group(CS_GRP_CALL):
            return self.emit_call(a, i)
        if m.startswith("set") and m[3:] in CCS:
            return self.wr(i, ops[0], "(%s?1u:0u)" % self.cond(m[3:]))
        if m.startswith("set") and ("j" + m[3:]) in CC_ALIAS:
            return self.wr(i, ops[0], "(%s?1u:0u)" % self.cond(CC_ALIAS["j" + m[3:]][1:]))
        h = getattr(self, "i_" + m.replace(" ", "_"), None)
        if h is None:
            raise Unsupported(m)
        return h(a, i, ops)

    # arithmetic -------------------------------------------------------
    def arith(self, a, i, ops, m):
        d, s = ops[0], ops[1]
        sz = d.size
        mk = MASK[sz]
        r = []
        r.append("ta=%s;tb=%s;" % (self.rd(i, d), self.rd(i, s, sz)))
        kind = {"add": "add", "adc": "add", "sub": "sub", "sbb": "sub", "cmp": "sub",
                "and": "logic", "or": "logic", "xor": "logic", "test": "logic"}[m]
        need = self.need
        if m == "adc":
            r.append("tw=(uint64_t)ta+tb+cf;tr=(uint32_t)tw&%s;" % mk)
            if need & CF:
                r.append("cf=(uint32_t)(tw>>%d)&1;" % BITS[sz])
            need &= ~CF
        elif m == "sbb":
            r.append("tw=(uint64_t)ta-tb-cf;tr=(uint32_t)tw&%s;" % mk)
            if need & CF:
                r.append("cf=(uint32_t)(tw>>63)&1;")
            need &= ~CF
        else:
            op = {"add": "+", "sub": "-", "cmp": "-", "and": "&", "or": "|", "xor": "^", "test": "&"}[m]
            r.append("tr=(ta%stb)&%s;" % (op, mk))
        if m not in ("cmp", "test"):
            r.append(self.wr(i, d, "tr"))
        if self.fuse_next and m in ("cmp", "sub", "test", "and", "or", "xor"):
            self.set_fused = ("sub" if kind == "sub" else "logic", sz)
        else:
            r.append(self.flags_from(kind, sz, need))
        return "".join(r)

    def i_add(self, a, i, o): return self.arith(a, i, o, "add")
    def i_adc(self, a, i, o): return self.arith(a, i, o, "adc")
    def i_sub(self, a, i, o): return self.arith(a, i, o, "sub")
    def i_sbb(self, a, i, o): return self.arith(a, i, o, "sbb")
    def i_cmp(self, a, i, o): return self.arith(a, i, o, "cmp")
    def i_and(self, a, i, o): return self.arith(a, i, o, "and")
    def i_or(self, a, i, o): return self.arith(a, i, o, "or")
    def i_xor(self, a, i, o): return self.arith(a, i, o, "xor")
    def i_test(self, a, i, o): return self.arith(a, i, o, "test")

    def incdec(self, a, i, ops, m):
        d = ops[0]
        sz = d.size
        r = "ta=%s;tr=(ta%s1u)&%s;" % (self.rd(i, d), "+" if m == "inc" else "-", MASK[sz])
        r += self.wr(i, d, "tr")
        r += self.flags_from(m, sz, self.need)
        return r

    def i_inc(self, a, i, o): return self.incdec(a, i, o, "inc")
    def i_dec(self, a, i, o): return self.incdec(a, i, o, "dec")

    def i_neg(self, a, i, ops):
        d = ops[0]
        sz = d.size
        r = "ta=%s;tr=(0u-ta)&%s;" % (self.rd(i, d), MASK[sz]) + self.wr(i, d, "tr")
        return r + self.flags_from("neg", sz, self.need)

    def i_not(self, a, i, ops):
        d = ops[0]
        return self.wr(i, d, "(~%s)&%s" % (self.rd(i, d), MASK[d.size]))

    def shift(self, a, i, ops, m):
        d = ops[0]
        sz = d.size
        b = BITS[sz]
        mk = MASK[sz]
        S = STY[sz]
        need = self.need
        if len(ops) == 1:
            cnt, const = "1u", 1
        elif ops[1].type == X.X86_OP_IMM:
            const = ops[1].imm & 31
            if const == 0:
                return ""
            cnt = "%du" % const
        else:
            cnt, const = "_n", None
        r = "ta=%s;" % self.rd(i, d)
        if m in ("shl", "sal"):
            r += "tw=(uint64_t)ta<<%s;tr=(uint32_t)tw&%s;" % (cnt, mk)
            fl = "cf=(uint32_t)(tw>>%d)&1;" % b if need & CF else ""
            if need & OF:
                fl += "of=((tr>>%d)&1)^((uint32_t)(tw>>%d)&1);" % (b - 1, b)
        elif m == "shr":
            r += "tr=(ta>>%s)&%s;" % (cnt, mk) if const != 32 else "tr=0;"
            fl = "cf=(ta>>(%s-1))&1;" % cnt if need & CF else ""
            if need & OF:
                fl += "of=(ta>>%d)&1;" % (b - 1)
        elif m == "sar":
            r += "tr=(uint32_t)((int32_t)(%s)ta>>%s)&%s;" % (S, cnt, mk)
            fl = "cf=(uint32_t)((int32_t)(%s)ta>>(%s-1))&1;" % (S, cnt) if need & CF else ""
            if need & OF:
                fl += "of=0;"
        elif m in ("rol", "ror"):
            c2 = "(%s%%%d)" % (cnt, b)
            if m == "rol":
                r += "tr=((ta<<%s)|(ta>>((%d-%s)%%%d)))&%s;" % (c2, b, c2, b, mk)
                fl = "cf=tr&1;" if need & CF else ""
                if need & OF:
                    fl += "of=((tr>>%d)&1)^(tr&1);" % (b - 1)
            else:
                r += "tr=((ta>>%s)|(ta<<((%d-%s)%%%d)))&%s;" % (c2, b, c2, b, mk)
                fl = "cf=(tr>>%d)&1;" % (b - 1) if need & CF else ""
                if need & OF:
                    fl += "of=((tr>>%d)^(tr>>%d))&1;" % (b - 1, b - 2)
            need &= CF | OF
        elif m in ("rcr", "rcl"):
            fn = "rcr" if m == "rcr" else "rcl"
            r += "{uint32_t _k=%s%%%d;while(_k--){" % (cnt, b + 1)
            if m == "rcr":
                r += "uint32_t _c=ta&1;ta=(ta>>1)|(cf<<%d);cf=_c;" % (b - 1)
            else:
                r += "uint32_t _c=(ta>>%d)&1;ta=((ta<<1)|cf)&%s;cf=_c;" % (b - 1, mk)
            r += "}tr=ta&%s;}" % mk
            fl = ""
            need = 0
        else:
            raise Unsupported(m)
        r += self.wr(i, d, "tr")
        if m not in ("rol", "ror", "rcr", "rcl"):
            fl += self.flags_from("logic", sz, need & (ZF | SF | PF))
        body = r + fl
        if const is None:
            return "{uint32_t _n=ecx&31;if(_n){%s}}" % body
        return body

    def i_shl(self, a, i, o): return self.shift(a, i, o, "shl")
    def i_sal(self, a, i, o): return self.shift(a, i, o, "shl")
    def i_shr(self, a, i, o): return self.shift(a, i, o, "shr")
    def i_sar(self, a, i, o): return self.shift(a, i, o, "sar")
    def i_rol(self, a, i, o): return self.shift(a, i, o, "rol")
    def i_ror(self, a, i, o): return self.shift(a, i, o, "ror")
    def i_rcr(self, a, i, o): return self.shift(a, i, o, "rcr")
    def i_rcl(self, a, i, o): return self.shift(a, i, o, "rcl")

    def i_shld(self, a, i, ops):
        d, s = ops[0], ops[1]
        cnt = "%du" % (ops[2].imm & 31) if ops[2].type == X.X86_OP_IMM else "(ecx&31)"
        return "{uint32_t _n=%s;if(_n){ta=%s;tb=%s;tr=(ta<<_n)|(tb>>(32-_n));cf=(ta>>(32-_n))&1;%s%s}}" % (
            cnt, self.rd(i, d), self.rd(i, s), self.wr(i, d, "tr"), self.flags_from("logic", 4, self.need & (ZF | SF | PF)))

    def i_shrd(self, a, i, ops):
        d, s = ops[0], ops[1]
        cnt = "%du" % (ops[2].imm & 31) if ops[2].type == X.X86_OP_IMM else "(ecx&31)"
        return "{uint32_t _n=%s;if(_n){ta=%s;tb=%s;tr=(ta>>_n)|(tb<<(32-_n));cf=(ta>>(_n-1))&1;%s%s}}" % (
            cnt, self.rd(i, d), self.rd(i, s), self.wr(i, d, "tr"), self.flags_from("logic", 4, self.need & (ZF | SF | PF)))

    def i_mul(self, a, i, ops):
        s = ops[0]
        sz = s.size
        if sz == 4:
            return "{uint64_t _p=(uint64_t)eax*%s;eax=(uint32_t)_p;edx=(uint32_t)(_p>>32);cf=of=(edx!=0);}" % self.rd(i, s)
        if sz == 1:
            return "{uint32_t _p=(eax&0xffu)*%s;eax=(eax&0xffff0000u)|(_p&0xffffu);cf=of=((_p>>8)!=0);}" % self.rd(i, s)
        return "{uint32_t _p=(eax&0xffffu)*%s;eax=(eax&0xffff0000u)|(_p&0xffffu);edx=(edx&0xffff0000u)|(_p>>16);cf=of=((_p>>16)!=0);}" % self.rd(i, s)

    def i_imul(self, a, i, ops):
        if len(ops) == 1:
            s = ops[0]
            sz = s.size
            if sz == 4:
                return "{int64_t _p=(int64_t)(int32_t)eax*(int32_t)%s;eax=(uint32_t)_p;edx=(uint32_t)((uint64_t)_p>>32);cf=of=(_p!=(int64_t)(int32_t)eax);}" % self.rd(i, s)
            if sz == 1:
                return "{int32_t _p=(int32_t)(int8_t)eax*(int8_t)%s;eax=(eax&0xffff0000u)|((uint32_t)_p&0xffffu);cf=of=(_p!=(int8_t)_p);}" % self.rd(i, s)
            return "{int32_t _p=(int32_t)(int16_t)eax*(int16_t)%s;eax=(eax&0xffff0000u)|((uint32_t)_p&0xffffu);edx=(edx&0xffff0000u)|(((uint32_t)_p>>16)&0xffffu);cf=of=(_p!=(int16_t)_p);}" % self.rd(i, s)
        d = ops[0]
        sz = d.size
        if len(ops) == 2:
            x, y = self.rd(i, d), self.rd(i, ops[1], sz)
        else:
            x, y = self.rd(i, ops[1]), self.rd(i, ops[2], sz)
        S = STY[sz]
        r = "{int64_t _p=(int64_t)(%s)%s*(int64_t)(%s)%s;tr=(uint32_t)_p&%s;%s" % (S, x, S, y, MASK[sz], self.wr(i, d, "tr"))
        if self.need & (CF | OF):
            r += "cf=of=(_p!=(int64_t)(%s)tr);" % S
        return r + "}"

    def i_div(self, a, i, ops):
        s = ops[0]
        sz = s.size
        if sz == 4:
            return "{uint32_t _d=%s;if(!_d)div_by_zero(%s);uint64_t _n=((uint64_t)edx<<32)|eax;eax=(uint32_t)(_n/_d);edx=(uint32_t)(_n%%_d);}" % (self.rd(i, s), hexs(a))
        if sz == 1:
            return "{uint32_t _d=%s;if(!_d)div_by_zero(%s);uint32_t _n=eax&0xffffu;eax=(eax&0xffff0000u)|((_n/_d)&0xffu)|(((_n%%_d)&0xffu)<<8);}" % (self.rd(i, s), hexs(a))
        return "{uint32_t _d=%s;if(!_d)div_by_zero(%s);uint32_t _n=((edx&0xffffu)<<16)|(eax&0xffffu);eax=(eax&0xffff0000u)|((_n/_d)&0xffffu);edx=(edx&0xffff0000u)|((_n%%_d)&0xffffu);}" % (self.rd(i, s), hexs(a))

    def i_idiv(self, a, i, ops):
        s = ops[0]
        sz = s.size
        if sz == 4:
            return "{int64_t _d=(int32_t)%s;if(!_d)div_by_zero(%s);int64_t _n=(int64_t)(((uint64_t)edx<<32)|eax);if(_d==-1&&_n==INT64_MIN)div_by_zero(%s);eax=(uint32_t)(_n/_d);edx=(uint32_t)(_n%%_d);}" % (self.rd(i, s), hexs(a), hexs(a))
        if sz == 1:
            return "{int32_t _d=(int8_t)%s;if(!_d)div_by_zero(%s);int32_t _n=(int16_t)(eax&0xffffu);eax=(eax&0xffff0000u)|((uint32_t)(_n/_d)&0xffu)|(((uint32_t)(_n%%_d)&0xffu)<<8);}" % (self.rd(i, s), hexs(a))
        return "{int32_t _d=(int16_t)%s;if(!_d)div_by_zero(%s);int32_t _n=(int32_t)(((edx&0xffffu)<<16)|(eax&0xffffu));eax=(eax&0xffff0000u)|((uint32_t)(_n/_d)&0xffffu);edx=(edx&0xffff0000u)|((uint32_t)(_n%%_d)&0xffffu);}" % (self.rd(i, s), hexs(a))

    # data movement ----------------------------------------------------
    def i_mov(self, a, i, ops):
        d, s = ops
        if s.type == X.X86_OP_REG and self.reg(i, s.reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
            return self.wr(i, d, "0x23u")
        if d.type == X.X86_OP_REG and self.reg(i, d.reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
            return ""
        return self.wr(i, d, self.rd(i, s, d.size))

    def i_movzx(self, a, i, ops):
        return self.wr(i, ops[0], self.rd(i, ops[1]))

    def i_movsx(self, a, i, ops):
        d, s = ops
        return self.wr(i, d, "(uint32_t)(int32_t)(%s)%s&%s" % (STY[s.size], self.rd(i, s), MASK[d.size]))

    def i_lea(self, a, i, ops):
        return self.wr(i, ops[0], self.addr(i, ops[1], seg=False))

    def i_xchg(self, a, i, ops):
        x, y = ops
        return "{uint32_t _x=%s,_y=%s;%s%s}" % (self.rd(i, x), self.rd(i, y), self.wr(i, x, "_y"), self.wr(i, y, "_x"))

    def i_push(self, a, i, ops):
        op = ops[0]
        if op.type == X.X86_OP_REG and self.reg(i, op.reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
            return "esp-=4;ST32(esp,0x3bu);"
        if op.size == 2:
            return "{uint32_t _v=%s;esp-=2;ST16(esp,_v);}" % self.rd(i, op)
        return "{uint32_t _v=%s;esp-=4;ST32(esp,_v);}" % self.rd(i, op, 4)

    def i_pop(self, a, i, ops):
        op = ops[0]
        if op.type == X.X86_OP_REG and self.reg(i, op.reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
            return "esp+=4;"
        if op.size == 2:
            return "{uint32_t _v=LD16(esp);esp+=2;%s}" % self.wr(i, op, "_v")
        return "{uint32_t _v=LD32(esp);esp+=4;%s}" % self.wr(i, op, "_v")

    def i_pushal(self, a, i, ops):
        return "{uint32_t _s=esp;" + "".join("esp-=4;ST32(esp,%s);" % r for r in ("eax", "ecx", "edx", "ebx", "_s", "ebp", "esi", "edi")) + "}"

    def i_popal(self, a, i, ops):
        return "".join("%s=LD32(esp);esp+=4;" % r if r else "esp+=4;" for r in ("edi", "esi", "ebp", None, "ebx", "edx", "ecx", "eax"))

    def i_pushfd(self, a, i, ops):
        return "{uint32_t _f=2u|cf|(pf<<2)|(zf<<6)|(sf<<7)|(g_cpu.df<<10)|(of<<11)|(g_cpu.eflags_extra&0x240000u);esp-=4;ST32(esp,_f);}"

    def i_popfd(self, a, i, ops):
        return "{uint32_t _f=LD32(esp);esp+=4;cf=_f&1;pf=(_f>>2)&1;zf=(_f>>6)&1;sf=(_f>>7)&1;of=(_f>>11)&1;g_cpu.df=(_f>>10)&1;g_cpu.eflags_extra=_f&0x240000u;}"

    def i_sahf(self, a, i, ops):
        return "cf=(eax>>8)&1;pf=(eax>>10)&1;zf=(eax>>14)&1;sf=(eax>>15)&1;"

    def i_lahf(self, a, i, ops):
        return "eax=(eax&0xffff00ffu)|((2u|cf|(pf<<2)|(zf<<6)|(sf<<7))<<8);"

    def i_leave(self, a, i, ops):
        return "esp=ebp;ebp=LD32(esp);esp+=4;"

    def i_cdq(self, a, i, ops):
        return "edx=(uint32_t)((int32_t)eax>>31);"

    def i_cwde(self, a, i, ops):
        return "eax=(uint32_t)(int32_t)(int16_t)eax;"

    def i_cbw(self, a, i, ops):
        return "eax=(eax&0xffff0000u)|((uint32_t)(int16_t)(int8_t)eax&0xffffu);"

    def i_cwd(self, a, i, ops):
        return "edx=(edx&0xffff0000u)|(((int16_t)eax<0)?0xffffu:0u);"

    def i_nop(self, a, i, ops): return ""
    def i_wait(self, a, i, ops): return ""
    def i_fwait(self, a, i, ops): return ""
    def i_cld(self, a, i, ops): return "g_cpu.df=0;"
    def i_std(self, a, i, ops): return "g_cpu.df=1;"
    def i_clc(self, a, i, ops): return "cf=0;"
    def i_stc(self, a, i, ops): return "cf=1;"
    def i_cmc(self, a, i, ops): return "cf^=1;"
    def i_int3(self, a, i, ops): return "TRAP(%s,\"int3\");" % hexs(a)
    def i_hlt(self, a, i, ops): return "TRAP(%s,\"hlt\");" % hexs(a)
    def i_ud2(self, a, i, ops): return "TRAP(%s,\"ud2\");" % hexs(a)
    def i_cpuid(self, a, i, ops): return "do_cpuid(&eax,&ebx,&ecx,&edx);"

    def i_rdtsc(self, a, i, ops):
        return "{uint64_t _t=do_rdtsc();eax=(uint32_t)_t;edx=(uint32_t)(_t>>32);}"

    def bitop(self, a, i, ops, m):
        d, s = ops
        need_set = m != "bt"
        if d.type == X.X86_OP_MEM and s.type == X.X86_OP_REG:
            A = self.addr(i, d)
            r = "{int32_t _bo=(int32_t)%s;uint32_t _ad=%s+(uint32_t)(_bo>>3);uint32_t _b=LD8(_ad);cf=(_b>>(_bo&7))&1;" % (self.rd(i, s, 4), A)
            if m == "bts": r += "ST8(_ad,_b|(1u<<(_bo&7)));"
            if m == "btr": r += "ST8(_ad,_b&~(1u<<(_bo&7)));"
            if m == "btc": r += "ST8(_ad,_b^(1u<<(_bo&7)));"
            return r + "}"
        b = BITS[d.size]
        r = "{uint32_t _v=%s;uint32_t _k=%s%%%d;cf=(_v>>_k)&1;" % (self.rd(i, d), self.rd(i, s, d.size), b)
        if m == "bts": r += self.wr(i, d, "_v|(1u<<_k)")
        if m == "btr": r += self.wr(i, d, "_v&~(1u<<_k)")
        if m == "btc": r += self.wr(i, d, "_v^(1u<<_k)")
        return r + "}"

    def i_bt(self, a, i, o): return self.bitop(a, i, o, "bt")
    def i_bts(self, a, i, o): return self.bitop(a, i, o, "bts")
    def i_btr(self, a, i, o): return self.bitop(a, i, o, "btr")
    def i_btc(self, a, i, o): return self.bitop(a, i, o, "btc")

    def i_bsf(self, a, i, ops):
        return "{uint32_t _v=%s;zf=(_v==0);if(_v){%s}}" % (self.rd(i, ops[1]), self.wr(i, ops[0], "__builtin_ctz(_v)"))

    def i_bsr(self, a, i, ops):
        return "{uint32_t _v=%s;zf=(_v==0);if(_v){%s}}" % (self.rd(i, ops[1]), self.wr(i, ops[0], "31-__builtin_clz(_v)"))

    # string ops -------------------------------------------------------
    def strsz(self, m):
        return {"b": 1, "w": 2, "d": 4}[m[-1]]

    def step(self, sz):
        return "(g_cpu.df?-%du:%du)" % (sz, sz)

    def stos(self, a, i, ops, rep):
        sz = self.strsz(i.mnemonic)
        if rep:
            return "rep_stos(&edi,&ecx,eax,%d);" % sz
        return self.st("edi", sz, "eax") + "edi+=%s;" % self.step(sz)

    def movs(self, a, i, ops, rep):
        sz = self.strsz(i.mnemonic)
        if rep:
            return "rep_movs(&edi,&esi,&ecx,%d);" % sz
        return self.st("edi", sz, self.ld("esi", sz)) + "edi+=%s;esi+=%s;" % (self.step(sz), self.step(sz))

    def cmps(self, a, i, ops, rep):
        sz = self.strsz(i.mnemonic)
        st = self.step(sz)
        fl = self.flags_from("sub", sz, self.need)
        if not rep:
            return "ta=%s;tb=%s;esi+=%s;edi+=%s;tr=(ta-tb)&%s;%s" % (self.ld("esi", sz), self.ld("edi", sz), st, st, MASK[sz], fl)
        brk = "!=" if rep == "repe" else "=="
        return "{uint32_t _n=ecx;if(_n){do{ta=%s;tb=%s;esi+=%s;edi+=%s;_n--;if(ta%stb)break;}while(_n);ecx=_n;tr=(ta-tb)&%s;%s}}" % (
            self.ld("esi", sz), self.ld("edi", sz), st, st, brk, MASK[sz], fl)

    def scas(self, a, i, ops, rep):
        sz = self.strsz(i.mnemonic)
        st = self.step(sz)
        fl = self.flags_from("sub", sz, self.need)
        if not rep:
            return "ta=eax&%s;tb=%s;edi+=%s;tr=(ta-tb)&%s;%s" % (MASK[sz], self.ld("edi", sz), st, MASK[sz], fl)
        brk = "!=" if rep == "repe" else "=="
        return "{uint32_t _n=ecx;if(_n){do{ta=eax&%s;tb=%s;edi+=%s;_n--;if(ta%stb)break;}while(_n);ecx=_n;tr=(ta-tb)&%s;%s}}" % (
            MASK[sz], self.ld("edi", sz), st, brk, MASK[sz], fl)

    def lods(self, a, i, ops, rep):
        sz = self.strsz(i.mnemonic)
        if rep:
            raise Unsupported("rep lods")
        return self.wreg({1: "al", 2: "ax", 4: "eax"}[sz], self.ld("esi", sz)) + "esi+=%s;" % self.step(sz)

    def __getattr__(self, name):
        if name.startswith("i_"):
            m = name[2:]
            rep = None
            for p in ("rep_", "repe_", "repne_", "repz_", "repnz_"):
                if m.startswith(p):
                    rep = {"rep_": "rep", "repe_": "repe", "repz_": "repe", "repne_": "repne", "repnz_": "repne"}[p]
                    m = m[len(p):]
            if len(m) >= 5 and m[:4] in ("stos", "movs", "cmps", "scas", "lods") and m[4] in "bwd" and len(m) == 5:
                if "xmm" in self._cur_opstr:
                    raise AttributeError(name)
                fn = getattr(FuncGen, m[:4])
                if m[:4] == "movs" and m == "movsd" and False:
                    pass
                return lambda a, i, o: fn(self, a, i, o, rep if m[:4] in ("cmps", "scas") else (rep is not None))
        raise AttributeError(name)

    def gen(self):
        f = self.f
        insns = f["insns"]
        order = sorted(insns)
        self.live_out, self.ud = flag_liveness(f)
        preds = compute_preds(f)
        self.labels_used = set()
        body = []
        idx_of = {a: k for k, a in enumerate(order)}
        self.fused = None
        for k, a in enumerate(order):
            i = insns[a]
            nxt = order[k + 1] if k + 1 < len(order) else None
            u, dfl = self.ud[a]
            self.need = dfl & self.live_out[a]
            # can the next instruction consume our flags directly?
            self.fuse_next = False
            self.set_fused = None
            fall = a + i.size
            if nxt == fall and len(preds.get(fall, ())) == 1 and fall in f["succ"][a]:
                j = insns[fall]
                jm = CC_ALIAS.get(j.mnemonic, j.mnemonic)
                cc = None
                if jm[0] == "j" and jm[1:] in CCS:
                    cc = jm[1:]
                elif jm.startswith("set") and jm[3:] in CCS:
                    cc = jm[3:]
                if cc is not None and i.mnemonic in ("cmp", "sub", "test", "and", "or", "xor"):
                    ju, jd = flag_use_def(j)
                    if (self.live_out[fall] & dfl) == 0 and (ju & ~dfl) == 0:
                        kind = "sub" if i.mnemonic in ("cmp", "sub") else "logic"
                        if self.fused_cond(kind, 4, cc) is not None:
                            self.fuse_next = True
            try:
                code = self.gen_insn(a, i)
            except Unsupported as e:
                code = "TRAP(%s,\"unsupported: %s %s\");" % (hexs(a), i.mnemonic, str(e).replace('"', "'"))
            self.fused = self.set_fused
            if self.fuse_next and self.set_fused is None:
                # fusion requested but not performed: materialise flags (shouldn't happen)
                pass
            body.append((a, i, code))
            # explicit fallthrough goto if layout doesn't match
            if fall in f["succ"][a] and not (i.mnemonic == "jmp") and nxt != fall:
                if fall in insns:
                    body.append((None, None, self.jump_to(fall)))
                else:
                    body.append((None, None, "bad_jump(%s,%s);" % (hexs(a), hexs(fall))))
        # labels: anything targeted by a non-layout edge
        out = []
        e = f["entry"]
        out.append("void F_%06x(void){" % e)
        out.append("REGS_DECL;uint32_t ta=0,tb=0,tr=0,cf=0,zf=0,sf=0,of=0,pf=0,fsw=0;uint64_t tw=0;")
        if self.uses_fpu and self.static:
            out.append("double f0=0,f1=0,f2=0,f3=0,f4=0,f5=0,f6=0,f7=0;")
        if e != order[0]:
            out.append("goto L_%06x;" % e)
            self.labels_used.add(e)
        for a, i, code in body:
            if a is not None and a in self.labels_used:
                out.append("L_%06x:" % a)
            if a is not None:
                out.append("/*%06x %s %s*/%s" % (a, i.mnemonic, i.op_str.replace("*/", "* /"), code))
            else:
                out.append(code)
        out.append("SYNC_OUT;return;}")
        return "\n".join(out)


class Unsupported(Exception):
    pass


# Guest functions replaced by native implementations in runtime/overrides.c
OVERRIDES = {
}


def main():
    import time
    t0 = time.time()
    P = Program().discover()
    fpu_program(P)
    P.used_imports = set()
    print("analysis %.1fs, %d functions" % (time.time() - t0, len(P.funcs)), file=sys.stderr)
    gdir = os.path.join(ROOT, "gen")
    os.makedirs(gdir, exist_ok=True)
    for fn in os.listdir(gdir):
        if fn.endswith(".c") or fn.endswith(".h"):
            os.remove(os.path.join(gdir, fn))
    entries = sorted(P.funcs)
    chunk, chunks, size = [], [], 0
    unsupported = collections.Counter()
    for e in entries:
        if e in OVERRIDES:
            chunk.append("void %s(void);\nvoid F_%06x(void){%s();}" % (OVERRIDES[e], e, OVERRIDES[e]))
            continue
        g = FuncGen(P, P.funcs[e])
        code = g.gen()
        for line in code.split("\n"):
            if "TRAP(" in line and "unsupported" in line:
                unsupported[line.split("unsupported: ")[1].split('"')[0]] += 1
        chunk.append(code)
        size += len(P.funcs[e]["insns"])
        if size > 12000:
            chunks.append(chunk)
            chunk, size = [], 0
    if chunk:
        chunks.append(chunk)
    with open(os.path.join(gdir, "funcs.h"), "w") as fh:
        fh.write('#pragma once\n#include "cpu.h"\n')
        for e in entries:
            fh.write("void F_%06x(void);\n" % e)
        for slot in sorted(P.img.imports):
            dll, nm = P.img.imports[slot]
            fh.write("void %s(void);\n" % imp_name(dll, nm))
    for k, ch in enumerate(chunks):
        with open(os.path.join(gdir, "code_%03d.c" % k), "w") as fh:
            fh.write('#include "funcs.h"\n#include "hle.h"\n')
            fh.write("\n\n".join(ch))
            fh.write("\n")
    with open(os.path.join(gdir, "disp.c"), "w") as fh:
        fh.write('#include "funcs.h"\n')
        fh.write("const struct { uint32_t a; gfn f; } g_disp_list[] = {\n")
        for e in entries:
            fh.write("{0x%06x,F_%06x},\n" % (e, e))
        fh.write("{0,0}};\n")
        fh.write("const struct { uint32_t slot; const char *dll; const char *name; gfn f; } g_imports[] = {\n")
        for slot in sorted(P.img.imports):
            dll, nm = P.img.imports[slot]
            fh.write('{0x%06x,"%s","%s",%s},\n' % (slot, dll, nm, imp_name(dll, nm)))
        fh.write("{0,0,0,0}};\n")
    print("generated %d files in %.1fs" % (len(chunks), time.time() - t0), file=sys.stderr)
    for k, v in unsupported.most_common():
        print("UNSUPPORTED", v, k, file=sys.stderr)


if __name__ == "__main__":
    main()
