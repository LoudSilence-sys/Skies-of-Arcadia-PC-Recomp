import struct,sys
def walk(F):
    n=struct.unpack_from('>I',F,8)[0]; st=12*n; out={}; idx=[]
    def name(i): o=struct.unpack_from('>I',F,12*i)[0]&0xFFFFFF; e=F.index(b'\0',st+o); return F[st+o:e].decode('latin1')
    def rec(first,last,dir):
        i=first
        while i<last:
            w0,a,b=struct.unpack_from('>III',F,12*i)
            p=dir+'/'+name(i)
            if w0>>24: rec(i+1,b,p); i=b
            else: out[p.lower()]=(i,a,b); i+=1
    rec(1,n,''); return out
ram=bytearray(open(sys.argv[1],'rb').read())
fa=struct.unpack_from('>I',ram,0x38)[0]&0x1FFFFFF; fn=struct.unpack_from('>I',ram,0x3c)[0]
F=bytes(ram[fa:fa+fn]); O=open(sys.argv[2],'rb').read()
a=walk(F); b=walk(O); ch=miss=0
for p,(i,off,sz) in a.items():
    if p in b:
        _,o2,s2=b[p]
        if (o2,s2)!=(off,sz): struct.pack_into('>II',ram,fa+12*i+4,o2,s2); ch+=1
    else: miss+=1
open(sys.argv[3],'wb').write(ram); print('files',len(a),'changed',ch,'not on our disc',miss)
