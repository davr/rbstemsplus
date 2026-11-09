import ctypes,sys,array,glob,os
M1_BR=[[0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,-1],[0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,-1],[0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,-1]]
M2_BR=[0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,-1]
M1_SR=[44100,48000,32000];M2_SR=[22050,24000,16000]
MASK=0xFFFE0CC0
def hdr_ok(b): return b[0]==0xFF and b[1]>=0xF0 and ((b[1]>>1)&3)!=0 and b[2]>=0x10 and (b[2]>>4)!=15 and ((b[2]>>2)&3)!=3
def fsize(b):
    ver=(b[1]>>3)&1; layer=(b[1]>>1)&3
    br=M1_BR[layer^3][b[2]>>4] if ver else M2_BR[b[2]>>4]
    sr=(M1_SR if ver else M2_SR)[(b[2]>>2)&3]
    spf=1152 if ver else 576
    return ((spf>>3)*br*1000)//sr+((b[2]>>1)&1)
def masked(b): return ((b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3])&MASK
def at4(d,pos): return d[pos:pos+4] if pos+4<=len(d) else None
def check_continuous(d,pos,mh):
    p=pos
    for k in range(3):
        b=at4(d,p)
        if b is None or d[p:p+3]==b"TAG": return 2
        if k<2:
            if not hdr_ok(b) or masked(b)!=mh: return 1
            p+=fsize(b)
        else:
            if b[0]!=0xFF or b[1]<0xF0 or not((b[1]>>1)&3) or ((b[2]>>2)&3)==3 or not(0x10<=b[2]<=0xEF) or masked(b)!=mh: return 1
    return 0
def check_xing(d,pos,side,st):
    a=pos+4+side
    if a+12>len(d): return -1
    b=d[a:a+12]
    if b[:4]==b"Xing" and (b[7]&1): st['xing']=1; return 0
    if b[:4]==b"Info": st['info']=1
    if st['xing']: return 0
    if a+18>len(d): return -1
    if side!=32:
        if pos+54>len(d): return -1
        b=d[pos+36:pos+40]
    if b[:4]==b"VBRI": st['xing']=1
    return 0
def analyze(d):
    F=[];samples=0;pos=0;first=True;mism=0;ref=None;st={'xing':0,'info':0};H=0
    while True:
        b=at4(d,pos)
        if b is None: break
        if d[pos:pos+3]==b"ID3":
            if pos+5>len(d): break
            vv=d[pos+3]*10+d[pos+4]
            if vv in(20,21,30,31,40):
                if pos+10>len(d): break
                s=d[pos+6:pos+10]
                n=(s[0]&0x7F)<<21|(s[1]&0x7F)<<14|(s[2]&0x7F)<<7|(s[3]&0x7F)
                pos=pos+n+10
            else: pos+=3
            continue
        if not hdr_ok(b): pos+=1; continue
        ver=(b[1]>>3)&1; layer=(b[1]>>1)&3; sri=(b[2]>>2)&3; chm=b[3]>>6
        spf=1152 if ver else 576
        if first:
            r=check_continuous(d,pos,masked(b))
            if r==1: pos+=1; continue
            if r==2: break
            size=fsize(b)
            side=(17 if chm==3 else 32) if ver else (9 if chm==3 else 17)
            if check_xing(d,pos,side,st)<0: break
            if st['xing'] or st['info']: H=spf
            F.append((pos,samples,size)); samples+=spf; pos+=size; first=False; ref=(ver,layer,sri,chm); continue
        size=fsize(b)
        if (ver,layer,sri,chm)==ref:
            F.append((pos,samples,size)); samples+=spf; pos+=size; mism=0; continue
        if len(F)<4:
            pos=F[-1][0]+1 if F else pos
            F=[];samples=0;mism=0;first=True;st={'xing':0,'info':0}; continue
        if mism>3: break
        mism+=1; pos+=1
    return F,H
class Mpg:
    def __init__(s,lib):
        s.d=ctypes.CDLL(lib)
        s.d.mpg123_init()
        s.d.mpg123_new.restype=ctypes.c_void_p
        for n in("mpg123_param","mpg123_format_none","mpg123_format","mpg123_open_feed","mpg123_feed","mpg123_decode_frame","mpg123_delete"): getattr(s.d,n).argtypes=None
        s.d.mpg123_param.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_long,ctypes.c_double]
        s.d.mpg123_format.argtypes=[ctypes.c_void_p,ctypes.c_long,ctypes.c_int,ctypes.c_int]
        s.d.mpg123_feed.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_size_t]
        s.d.mpg123_decode_frame.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(ctypes.c_size_t)]
        s.d.mpg123_open_feed.argtypes=[ctypes.c_void_p]; s.d.mpg123_format_none.argtypes=[ctypes.c_void_p]; s.d.mpg123_delete.argtypes=[ctypes.c_void_p]
    def feed_decode(s,h,d,f):
        off,start,size=f
        ln=min(size,0x2000)
        if off>=len(d): ln=0
        elif ln>len(d)-off: ln=len(d)-off
        s.d.mpg123_feed(h,d[off:off+ln],ln)
        num=ctypes.c_int64(0); pcm=ctypes.c_void_p(); nb=ctypes.c_size_t(0)
        r=s.d.mpg123_decode_frame(h,ctypes.byref(num),ctypes.byref(pcm),ctypes.byref(nb))
        data=ctypes.string_at(pcm.value,nb.value) if pcm.value and nb.value else b""
        return r,data
def rb_mp3(path,lib=r"C:\Program Files\rekordbox\rekordbox 7.2.14\libmpg123.dll"):
    d=open(path,'rb').read()
    F,H=analyze(d)
    if not F: raise SystemExit("no frame map")
    b=d[F[0][0]:F[0][0]+4]
    ver=(b[1]>>3)&1; rate=(M1_SR if ver else M2_SR)[(b[2]>>2)&3]; ch=1 if (b[3]>>6)==3 else 2; spf=1152 if ver else 576
    m=Mpg(lib)
    err=ctypes.c_int(0)
    m.d.mpg123_new.argtypes=[ctypes.c_char_p,ctypes.POINTER(ctypes.c_int)]
    h=m.d.mpg123_new(None,ctypes.byref(err))
    m.d.mpg123_param(h,0,0,0.0); m.d.mpg123_param(h,1,0x300,0.0); m.d.mpg123_param(h,15,-1,0.0)
    m.d.mpg123_format_none(h); m.d.mpg123_format(h,rate,ch,0xd0)
    m.d.mpg123_open_feed(h)
    for f in F:
        if f[1]>=H:
            r,_=m.feed_decode(h,d,f)
            if r!=-10: break
    m.d.mpg123_open_feed(h)
    n=len(F)*spf; fc=0
    s16=array.array('h',[0])*(n*ch)
    for i,f in enumerate(F):
        if f[1]<H: continue
        r,pcm=m.feed_decode(h,d,f)
        if r==-10 and i+1<len(F): fc+=F[i+1][1]-f[1]; continue
        if len(pcm)!=spf*ch*2: raise SystemExit(f"frame {i} decodes to {len(pcm)} bytes")
        p=f[1]-fc
        s16[p*ch:(p+spf)*ch]=array.array('h',pcm)
    m.d.mpg123_delete(h)
    return s16,ch,n,rate,H,len(F)
if __name__=="__main__":
    L=485100;HOP=483052;PRE=1024
    path=sys.argv[1]; first=int(sys.argv[2]); cnt=int(sys.argv[3])
    s16,ch,n,rate,H,nf=rb_mp3(path)
    print("frames",nf,"H",H,"rate",rate,"ch",ch,"samples",n)
    dump=sorted(glob.glob(r"C:\rbstems-dump\*.mix"))[first:first+cnt]
    for i,p in enumerate(dump):
        ref=array.array('f'); ref.frombytes(open(p,'rb').read())
        lo=i*HOP-PRE
        exact=True;maxd=0.0
        for c in range(2):
            for j in range(L):
                k=lo+j
                v=0.0
                if 0<=k<n-0: v=s16[k*ch+(ch-1 if c else 0)]/32768.0
                if k==n-1: v=0.0
                dlt=abs(v-ref[c*L+j])
                if dlt>maxd: maxd=dlt
        print(f"chunk {i}: max|diff|={maxd:.3e}","EXACT" if maxd==0 else "")
