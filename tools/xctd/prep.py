import struct,sys
src,dst=sys.argv[1],sys.argv[2]
d=open(src,"rb").read()
assert d[:4]==b"\x0f\xf5\x12\xed"
flags,=struct.unpack(">I",d[12:16]); usize,=struct.unpack("<I",d[16:20])
p=0x14; raw=b""
while p<len(d):
    if d[p]==0xFF: u,c=struct.unpack(">HH",d[p+1:p+5]); p+=5
    else: c,=struct.unpack(">H",d[p:p+2]); p+=2
    raw+=d[p:p+c]; p+=c
open(dst,"wb").write(raw)
print(15+((flags>>4)&3), hex(usize), "end_ok" if p==len(d) else "END_MISMATCH")
