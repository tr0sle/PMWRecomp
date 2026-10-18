import json, collections, sys
from pe import Image
from capstone import x86 as X, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET
img=Image()
procs=json.load(open('../analysis/ghidra_procs.json'))
seeds=set(int(p['address'],16) for p in procs if not p['procedure']['external'] and p['address'].startswith('0x'))
seeds.add(img.entry)
seen=set(); mn=collections.Counter(); ind_jmp=[]; ind_call=collections.Counter(); seg=collections.Counter(); bad=[]
work=list(seeds)
while work:
    a=work.pop()
    while a not in seen:
        if not img.in_text(a): bad.append(a); break
        i=img.insn(a)
        if i is None: bad.append(a); break
        seen.add(a)
        pfx=[p for p in i.prefix if p]
        key=i.mnemonic
        mn[key]+=1
        for op in i.operands:
            if op.type==X.X86_OP_MEM and op.mem.segment!=0: seg[(i.mnemonic,i.reg_name(op.mem.segment))]+=1
        nxt=a+i.size
        if i.group(CS_GRP_RET) or i.mnemonic in('hlt','int3','ud2'): break
        if i.group(CS_GRP_JUMP):
            op=i.operands[0]
            if op.type==X.X86_OP_IMM:
                work.append(op.imm)
                if i.mnemonic=='jmp': break
            else:
                ind_jmp.append((a,i.mnemonic,i.op_str)); break
        if i.group(CS_GRP_CALL):
            op=i.operands[0]
            if op.type==X.X86_OP_IMM: work.append(op.imm)
            else: ind_call[i.op_str.split('[')[0]]+=1
        a=nxt
print("insns",len(seen),"bad",len(bad), [hex(b) for b in bad[:20]])
for k,v in sorted(mn.items(), key=lambda x:-x[1]): print(f"{k}:{v}", end="  ")
print()
print("seg",seg)
print("indjmp",len(ind_jmp))
for x in ind_jmp[:60]: print(hex(x[0]),x[1],x[2])
json.dump(sorted(seen),open('../analysis/seen.json','w'))
print("---- non-table indirect jumps")
for x in ind_jmp:
    if '*4 +' not in x[2] and not x[2].startswith('dword ptr [0x4f8'): print(hex(x[0]),x[1],x[2])
