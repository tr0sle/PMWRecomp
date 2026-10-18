"""Function discovery, CFG construction, jump tables, flag liveness and x87 depth analysis."""
import json, os, collections
from pe import Image
from capstone import x86 as X, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CF, PF, ZF, SF, OF = 1, 2, 4, 8, 16
ALLF = CF | PF | ZF | SF | OF

JCC_USE = {
    "jo": OF, "jno": OF, "jb": CF, "jae": CF, "je": ZF, "jne": ZF, "jbe": CF | ZF, "ja": CF | ZF,
    "js": SF, "jns": SF, "jp": PF, "jnp": PF, "jl": SF | OF, "jge": SF | OF, "jle": ZF | SF | OF, "jg": ZF | SF | OF,
}
SETCC_USE = {"set" + k[1:]: v for k, v in JCC_USE.items()}

# mnemonic -> (use, def)
FLAG_TAB = {}
for m in ("add", "sub", "cmp", "neg", "and", "or", "xor", "test"):
    FLAG_TAB[m] = (0, ALLF)
for m in ("adc", "sbb"):
    FLAG_TAB[m] = (CF, ALLF)
for m in ("inc", "dec"):
    FLAG_TAB[m] = (0, ALLF & ~CF)
for m in ("mul", "imul", "div", "idiv"):
    FLAG_TAB[m] = (0, ALLF)
for m in ("bt", "bts", "btr", "btc"):
    FLAG_TAB[m] = (0, CF)
FLAG_TAB["rcr"] = (CF, CF | OF)
FLAG_TAB["rcl"] = (CF, CF | OF)
FLAG_TAB["pushfd"] = (ALLF, 0)
FLAG_TAB["popfd"] = (0, ALLF)
FLAG_TAB["sahf"] = (0, ALLF & ~OF)
FLAG_TAB["lahf"] = (ALLF & ~OF, 0)
FLAG_TAB["stc"] = (0, CF)
FLAG_TAB["clc"] = (0, CF)
FLAG_TAB["cmc"] = (CF, CF)
for m in ("repe cmpsb", "repe cmpsd", "repne scasb", "repe cmpsw", "repne scasd", "cmpsb", "cmpsd", "scasb", "scasd",
          "repne cmpsb", "repe scasb", "repe scasd"):
    FLAG_TAB[m] = (ALLF, ALLF)  # rep with ecx==0 leaves flags: keep them live (use) to be safe
for k, v in JCC_USE.items():
    FLAG_TAB[k] = (v, 0)
for k, v in SETCC_USE.items():
    FLAG_TAB[k] = (v, 0)
# shifts by immediate kill all flags (count != 0); by cl handled specially


def shift_flags(i):
    ops = i.operands
    if len(ops) == 2 and ops[1].type == X.X86_OP_IMM:
        if (ops[1].imm & 31) == 0:
            return (0, 0)
        return (0, ALLF)
    if len(ops) == 1:  # shift by 1
        return (0, ALLF)
    return (ALLF, ALLF)  # by cl: may leave flags unchanged -> conservatively use all


def flag_use_def(i):
    m = i.mnemonic
    if m in ("shl", "sal", "shr", "sar", "rol", "ror"):
        return shift_flags(i)
    if m in ("shld", "shrd"):
        return (ALLF, ALLF)
    return FLAG_TAB.get(m, (0, 0))


def mem_target(op):
    """absolute [disp] memory operand -> disp or None"""
    if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and op.mem.segment == 0:
        return op.mem.disp & 0xFFFFFFFF
    return None


class Program:
    def __init__(self):
        self.img = Image()
        self.entries = set()
        self.funcs = {}
        self.extra_seeds = set()

    # ------------------------------------------------------------------ seeds
    def initial_seeds(self):
        img = self.img
        procs = json.load(open(os.path.join(ROOT, "analysis", "ghidra_procs.json")))
        s = set()
        for p in procs:
            if p["procedure"]["external"] or not p["address"].startswith("0x"):
                continue
            a = int(p["address"], 16)
            if img.in_text(a):
                s.add(a)
        s.add(img.entry)
        # aligned code pointers stored in data sections
        for name, lo, hi, ch in img.sections:
            if name in (".text", ".rsrc"):
                continue
            for a in range(lo, hi - 3, 4):
                if a - img.base + 4 > len(img.mem):
                    break
                v = img.u32(a)
                if img.in_text(v) and self.plausible_code(v):
                    s.add(v)
        extra = os.path.join(ROOT, "analysis", "extra_seeds.txt")
        if os.path.exists(extra):
            for line in open(extra):
                line = line.split("#")[0].strip()
                if line:
                    s.add(int(line, 16))
        return s

    def plausible_code(self, a, n=6):
        img = self.img
        for _ in range(n):
            i = img.insn(a)
            if i is None:
                return False
            if i.mnemonic in ("int3",) and _ == 0:
                return False
            if i.group(CS_GRP_RET) or i.mnemonic == "jmp":
                return True
            a += i.size
        return True

    # ------------------------------------------------------------- traversal
    def jump_table(self, a, i, prevmap):
        img = self.img
        op = i.operands[0]
        T = op.mem.disp & 0xFFFFFFFF
        idx = i.reg_name(op.mem.index)
        n = None
        # search backwards for bounds check
        p = prevmap.get(a)
        steps = 0
        bytetab = None
        cur_idx = idx
        while p is not None and steps < 10:
            pi = img.insn(p)
            if pi.mnemonic == "movzx" and len(pi.operands) == 2 and pi.reg_name(pi.operands[0].reg) == cur_idx \
                    and pi.operands[1].type == X.X86_OP_MEM and pi.operands[1].size == 1 and pi.operands[1].mem.scale == 1:
                m = pi.operands[1].mem
                bytetab = m.disp & 0xFFFFFFFF
                cur_idx = pi.reg_name(m.index) if m.index else pi.reg_name(m.base)
            elif pi.mnemonic == "cmp" and pi.operands[0].type == X.X86_OP_REG and pi.reg_name(pi.operands[0].reg) == cur_idx \
                    and pi.operands[1].type == X.X86_OP_IMM:
                n = (pi.operands[1].imm & 0xFFFFFFFF) + 1
                break
            elif pi.mnemonic == "and" and pi.operands[0].type == X.X86_OP_REG and pi.reg_name(pi.operands[0].reg) == cur_idx \
                    and pi.operands[1].type == X.X86_OP_IMM and bytetab is None:
                n = (pi.operands[1].imm & 0xFFFFFFFF) + 1  # masked index (memcpy alignment tables)
                break
            p = prevmap.get(p)
            steps += 1
        if n is not None and n > 4096:
            n = None
        if bytetab is not None and n is not None:
            n = max(img.u8(bytetab + k) for k in range(n)) + 1
        targets = []
        if n is None:
            # unbounded index (e.g. negative offsets into a group of tables): take plausible entries before T too
            k = 1
            while k <= 64:
                v = img.u32(T - 4 * k)
                if not img.in_text(v) or abs(v - a) > 0x4000:
                    break
                targets.append(v)
                k += 1
        k = 0
        while True:
            if n is not None and k >= n:
                break
            if n is None and k >= 512:
                break
            v = img.u32(T + 4 * k)
            if not img.in_text(v) or (n is None and abs(v - a) > 0x4000):
                if n is None:
                    break
                k += 1
                continue
            targets.append(v)
            k += 1
        return T, targets

    def build(self, entry, final=False):
        img = self.img
        f = {"entry": entry, "insns": {}, "succ": {}, "jt": {}, "calls": set(), "tails": set(), "bad": [], "tailjmp": {}}
        insns = f["insns"]
        prevmap = {}
        work = [(entry, None)]
        while work:
            a, prev = work.pop()
            if a in insns:
                continue
            if not img.in_text(a):
                f["bad"].append(a)
                continue
            i = img.insn(a)
            if i is None:
                f["bad"].append(a)
                continue
            insns[a] = i
            if prev is not None:
                prevmap[a] = prev
            m = i.mnemonic
            nxt = a + i.size
            succ = []
            if i.group(CS_GRP_RET) or m in ("int3", "hlt", "ud2"):
                pass
            elif i.group(CS_GRP_JUMP):
                op = i.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    if final and t in self.entries and t != entry:
                        f["tails"].add(t)
                        f["tailjmp"][a] = t
                    else:
                        succ.append(t)
                    if m != "jmp":
                        succ.append(nxt)
                elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index != 0 and op.mem.scale == 4 and m == "jmp":
                    T, targets = self.jump_table(a, i, prevmap)
                    f["jt"][a] = (T, targets)
                    for t in targets:
                        if final and t in self.entries and t != entry:
                            f["tails"].add(t)
                        elif t not in succ:
                            succ.append(t)
            elif i.group(CS_GRP_CALL):
                op = i.operands[0]
                if op.type == X.X86_OP_IMM:
                    f["calls"].add(op.imm & 0xFFFFFFFF)
                succ.append(nxt)
            else:
                succ.append(nxt)
            f["succ"][a] = succ
            for s in succ:
                work.append((s, a if s == nxt else None))
        return f

    def discover(self):
        seeds = self.initial_seeds()
        # also immediates referring to code
        self.entries = set(seeds)
        work = list(seeds)
        done = set()
        while work:
            e = work.pop()
            if e in done:
                continue
            done.add(e)
            f = self.build(e)
            for c in f["calls"]:
                if self.img.in_text(c) and c not in self.entries:
                    self.entries.add(c)
                    work.append(c)
            for a, i in f["insns"].items():
                for op in i.operands:
                    if op.type == X.X86_OP_IMM and i.mnemonic in ("push", "mov"):
                        v = op.imm & 0xFFFFFFFF
                        if self.img.in_text(v) and v not in self.entries and self.plausible_code(v):
                            self.entries.add(v)
                            work.append(v)
        # final build with tail-call awareness
        for e in sorted(self.entries):
            self.funcs[e] = self.build(e, final=True)
        return self


def compute_preds(f):
    preds = collections.defaultdict(list)
    for a, ss in f["succ"].items():
        for s in ss:
            preds[s].append(a)
    return preds


def flag_liveness(f):
    """returns live_out map: addr -> flags needed after instruction"""
    insns = f["insns"]
    ud = {a: flag_use_def(i) for a, i in insns.items()}
    for a, i in insns.items():
        if i.group(CS_GRP_CALL):
            ud[a] = (0, ALLF)
    live_in = {a: 0 for a in insns}
    live_out = {a: 0 for a in insns}
    order = sorted(insns, reverse=True)
    changed = True
    while changed:
        changed = False
        for a in order:
            out = 0
            for s in f["succ"][a]:
                out |= live_in.get(s, ALLF)  # unknown successor: conservative
            u, d = ud[a]
            inn = u | (out & ~d)
            if out != live_out[a] or inn != live_in[a]:
                live_out[a] = out
                live_in[a] = inn
                changed = True
    return live_out, ud


# ---------------------------------------------------------------- x87 effects
def fpu_effect(i):
    """(required_depth, delta) for x87 instruction; None if not x87"""
    b = i.bytes
    k = 0
    while b[k] in (0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0xF0):
        k += 1
    op = b[k]
    if op == 0x9B:
        return (0, 0)
    if not (0xD8 <= op <= 0xDF):
        return None
    modrm = b[k + 1]
    mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
    if mod != 3:
        if op in (0xD8, 0xDC, 0xDA, 0xDE):
            return (1, -1 if reg == 3 else 0)  # arith/compare with mem; fcomp pops
        if op in (0xD9,):
            return {0: (0, 1), 2: (1, 0), 3: (1, -1)}.get(reg, (0, 0))
        if op == 0xDB:
            return {0: (0, 1), 1: (1, -1), 2: (1, 0), 3: (1, -1), 5: (0, 1), 7: (1, -1)}.get(reg, (0, 0))
        if op == 0xDD:
            return {0: (0, 1), 1: (1, -1), 2: (1, 0), 3: (1, -1)}.get(reg, (0, 0))
        if op == 0xDF:
            return {0: (0, 1), 1: (1, -1), 2: (1, 0), 3: (1, -1), 5: (0, 1), 7: (1, -1)}.get(reg, (0, 0))
        return (0, 0)
    sti = rm
    if op == 0xD8:
        return (max(1, sti + 1), -1 if reg == 3 else 0)
    if op == 0xD9:
        if reg == 0:
            return (sti + 1, 1)  # fld st(i)
        if reg == 1:
            return (sti + 1 if sti else 1, 0)  # fxch
        x = modrm
        if x == 0xD0:
            return (0, 0)
        if x in (0xE0, 0xE1, 0xE4, 0xE5, 0xFA, 0xFC, 0xFE, 0xFF, 0xF0):
            return (1, 0)
        if 0xE8 <= x <= 0xEE:
            return (0, 1)
        if x in (0xF2,):
            return (1, 1)  # fptan pushes 1.0
        if x in (0xFB,):
            return (1, 1)  # fsincos
        if x in (0xF1, 0xF3, 0xF9):
            return (2, -1)  # fyl2x, fpatan
        if x in (0xF8, 0xF5, 0xFD):
            return (2, 0)  # fprem, fscale
        if x in (0xF4,):
            return (1, 1)
        if x in (0xF6, 0xF7):
            return (0, 0)
        return (0, 0)
    if op == 0xDA:
        if modrm == 0xE9:
            return (2, -2)
        return (sti + 1, 0)
    if op == 0xDB:
        return (sti + 1 if reg in (0, 1, 2, 3, 5, 6) else 0, 0)
    if op == 0xDC:
        return (max(1, sti + 1), 0)
    if op == 0xDD:
        if reg == 0:
            return (0, 0)  # ffree
        if reg == 2:
            return (max(1, sti + 1), 0)
        if reg == 3:
            return (max(1, sti + 1), -1)
        if reg == 4:
            return (max(1, sti + 1), 0)
        if reg == 5:
            return (max(1, sti + 1), -1)
        return (0, 0)
    if op == 0xDE:
        if modrm == 0xD9:
            return (2, -2)
        return (max(2, sti + 1), -1)
    if op == 0xDF:
        if modrm == 0xE0:
            return (0, 0)
        if reg in (5, 6):
            return (max(1, sti + 1), -1)  # fucomip/fcomip
        return (0, 0)
    return (0, 0)


def fpu_analysis(f, call_float, call_delta, start=0):
    """Forward x87 depth propagation.
    call_delta: callee address -> (required, delta) for direct calls with known effect.
    call_float: per-call-site override (indirect calls returning ST0), learned from underflows.
    Returns (mode, depth_in map, ret_depths, reason)."""
    insns = f["insns"]
    for _round in range(64):
        depth = {f["entry"]: (start, None, 0, 0)}
        work = [f["entry"]]
        conflict = None
        underflow = None
        ret_depths = set()
        while work:
            a = work.pop()
            d, lastcall, dcall, dmin = depth[a]
            i = insns[a]
            eff = fpu_effect(i)
            nd, nlast, ndcall, ndmin = d, lastcall, dcall, dmin
            if eff is None and i.group(CS_GRP_CALL):
                op = i.operands[0]
                t = op.imm & 0xFFFFFFFF if op.type == X.X86_OP_IMM else None
                if t is not None and call_delta.get(t) is not None:
                    eff = call_delta[t]
                elif call_float.get(a):
                    eff = (0, 1)
                else:
                    eff = (0, 0)
                    if t is None or t in call_delta:
                        nlast, ndcall, ndmin = a, d, d
            if eff is not None:
                req, delta = eff
                if d < req:
                    underflow = (a, lastcall if (lastcall is not None and dmin >= dcall and d - req == -1 + 0 * dcall) else None)
                    break
                nd = d + delta
                if lastcall is not None:
                    ndmin = min(dmin, d - req, nd)
            if i.group(CS_GRP_RET):
                ret_depths.add(d)
            elif a in f.get("tailsites", ()):
                ret_depths.add(("?", d))
            if a in f["tailjmp"]:
                t = f["tailjmp"][a]
                ce = call_delta.get(t, (0, 0))
                if ce is None:
                    ret_depths.add(("?", d))
                else:
                    if d < ce[0]:
                        underflow = (a, None)
                        break
                    ret_depths.add(d + ce[1])
            if nd < 0 or nd > 8:
                conflict = a
                break
            for s in f["succ"][a]:
                if s not in insns:
                    continue
                if s in depth:
                    if depth[s][0] != nd:
                        conflict = s
                        break
                else:
                    depth[s] = (nd, nlast, ndcall, ndmin)
                    work.append(s)
            if conflict:
                break
        if underflow:
            a, lastcall = underflow
            if lastcall is not None and not call_float.get(lastcall):
                call_float[lastcall] = True
                continue
            return ("dynamic", None, None, "args@%x" % a)
        if conflict:
            return ("dynamic", None, None, "conflict@%x" % conflict)
        return ("static", {k: v[0] for k, v in depth.items()}, ret_depths, None)
    return ("dynamic", None, None, "rounds")


INTRINSIC_FPU = {0x4F3718: (1, -1), 0x4C4D2C: (1, -1)}  # __ftol, __ftol2


def fpu_program(P):
    """Whole-program x87 analysis; sets f['fpu'] = (mode, depthmap, retdepth) and call-site info."""
    call_delta = dict(INTRINSIC_FPU)
    call_float = {}
    for e, f in P.funcs.items():
        f["tailsites"] = set(a for a, i in f["insns"].items()
                             if (i.mnemonic == "jmp" and a not in f["jt"] and i.operands[0].type != X.X86_OP_IMM))
    for it in range(20):
        changed = False
        res = {}
        call_float.clear()
        for e, f in P.funcs.items():
            if e in INTRINSIC_FPU:
                continue
            r = fpu_analysis(f, call_float, call_delta)
            if r[0] == "dynamic":
                # function taking x87 args: analyse with virtual depth to get its net effect
                K = 4
                r2 = fpu_analysis(f, call_float, call_delta, start=K)
                known2 = [x for x in (r2[2] or ()) if not isinstance(x, tuple)]
                if r2[0] == "static" and len(set(known2)) == 1:
                    rd = known2[0]
                    req = 0
                    for a2, d2 in r2[1].items():
                        e2 = fpu_effect(f["insns"][a2])
                        if e2:
                            req = max(req, K - d2 + e2[0])
                    eff = (req, rd - K)
                else:
                    eff = None
                res[e] = ("dynamic", None, None, r[3], eff)
            else:
                rds = r[2]
                known = [x for x in rds if not isinstance(x, tuple)]
                if known:
                    rd = max(known)
                    eff = (0, rd)
                elif rds:
                    eff = None  # only indirect tail exits: unknown
                else:
                    eff = (0, 0)
                res[e] = ("static", r[1], rds, None, eff)
        for e, r in res.items():
            eff = r[4]
            if eff != (0, 0):
                if e not in call_delta or call_delta[e] != eff:
                    call_delta[e] = eff
                    changed = True
            elif e in call_delta and e not in INTRINSIC_FPU:
                del call_delta[e]
                changed = True
        if not changed:
            break
    for e, f in P.funcs.items():
        if e in INTRINSIC_FPU:
            f["fpu"] = ("dynamic", None, None, "intrinsic", INTRINSIC_FPU[e])
        else:
            f["fpu"] = res[e]
    P.call_delta = call_delta
    P.call_float = call_float
    return res
