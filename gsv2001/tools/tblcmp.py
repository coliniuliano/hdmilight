#!/usr/bin/env python3
"""tblcmp.py REF OTHER...: find the Gsv2k1 init table (3-byte page/reg/value entries, ff-terminated) in each image by an
anchor from the reference and compare entry lists."""
import sys,difflib,re
def table(img,anchor=bytes.fromhex('00feff00fe0000ff7f00ff00')):
    out=[]
    for m in re.finditer(re.escape(anchor),img):
        o=m.start()
        # walk back to the table start: entries are 3 bytes; the ref starts 6 bytes before the anchor
        s=o-6; e=s; ent=[]
        while e+3<=len(img) and img[e]!=0xff and len(ent)<400:
            ent.append(tuple(img[e:e+3])); e+=3
        out.append((s,ent))
    return out
ref=open(sys.argv[1],'rb').read(); rt=table(ref)
print('ref tables at',[(hex(s),len(e)) for s,e in rt])
r=rt[0][1]
for p in sys.argv[2:]:
    img=open(p,'rb').read()
    for s,e in table(img):
        sm=difflib.SequenceMatcher(None,r,e,autojunk=False)
        same=sum(b.size for b in sm.get_matching_blocks())
        print('\n%s: table at 0x%x, %d entries; %d of ref %d in same order'%(p.split('/')[-1],s,len(e),same,len(r)))
        for tag,i1,i2,j1,j2 in sm.get_opcodes():
            if tag=='equal': continue
            print('  %-7s ref[%d:%d] %s | other %s'%(tag,i1,i2,' '.join('%02x.%02x=%02x'%x for x in r[i1:i2]),' '.join('%02x.%02x=%02x'%x for x in e[j1:j2])))
