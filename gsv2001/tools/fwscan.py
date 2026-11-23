#!/usr/bin/env python3
"""fwscan.py FILE...: flatten Intel HEX (or take a raw .bin), then report what Gscoolink chip code is inside:
device descriptors 0x0001XXpp found as 32-bit literals (which I2C addresses and register pages), and telling strings."""
import sys,struct,re,collections,os
def ihex(path):
    mem={}; base=0
    for line in open(path,'r',errors='replace'):
        line=line.strip()
        if not line.startswith(':'): continue
        b=bytes.fromhex(line[1:]); n,addr,typ=b[0],(b[1]<<8)|b[2],b[3]; data=b[4:4+n]
        if typ==0:
            for i,x in enumerate(data): mem[base+addr+i]=x
        elif typ==4: base=((data[0]<<8)|data[1])<<16
        elif typ==2: base=((data[0]<<8)|data[1])<<4
    lo=min(mem); hi=max(mem)
    out=bytearray(b'\xff'*(hi-lo+1))
    for a,x in mem.items(): out[a-lo]=x
    return lo,bytes(out)
for path in sys.argv[1:]:
    if path.lower().endswith('.hex'): base,img=ihex(path)
    else: base,img=None,open(path,'rb').read()
    print('=====',os.path.basename(path),'base',hex(base) if base is not None else '?','size',len(img))
    open(path+'.flat','wb').write(img)
    words=struct.unpack('<%dI'%(len(img)//4),img[:len(img)//4*4])
    w2=struct.unpack('<%dI'%((len(img)-2)//4),img[2:2+(len(img)-2)//4*4])
    c=collections.Counter()
    for w in list(words)+list(w2):
        if (w>>16)==1 and ((w>>8)&0xff) in (0xb0,0xb2,0xb4,0xb6,0xb8,0xba,0xbc,0xbe,0x98,0x9a,0xa0): c[w]+=1
    by=collections.defaultdict(list)
    for w,n in sorted(c.items()): by[(w>>8)&0xff].append('%02x'%(w&0xff))
    for a,p in by.items(): print('  descriptors dev 0x%02x pages: %s'%(a,' '.join(p)))
    ss=re.findall(rb'[\x20-\x7e]{5,}',img)
    pat=re.compile(rb'gsv|GSV|Gsv|2k\d|6k\d|Av[A-Z][a-z]+|Port ?(Rx|Tx)|Rx ?%d|Tx ?%d|DEMO|Scaler|scal|DownSc|HDCP|EZ-|EZCOO|Ver|V\d\.\d|IT6|EP9|MS\d{4}|LT\d{4}|RTD|Audio/Video Software',re.I)
    hits=[s.decode() for s in ss if pat.search(s)]
    print('  %d strings, %d matching; samples:'%(len(ss),len(hits)))
    seen=set()
    for h in hits:
        k=h[:40]
        if k in seen: continue
        seen.add(k)
        if len(seen)>45: break
        print('    ',h[:110])
