import numpy as np, glob, subprocess, sys, os, math, itertools
from fractions import Fraction
TAPS=29;HIST=28
def fma_d(a,b,c): return float(Fraction(a)*Fraction(b)+Fraction(c))
def build(L,M,fc,beta,fma_t=True,fma_s=True,scale_first=False):
    N=29*L; half=N//2
    twopi_fc=fc*6.283185307179586; two_fc=fc+fc; inv=1.0/(N-1); step=inv+inv; hb=beta*0.5
    fact=[1.0]*22
    for k in range(2,22): fact[k]=fact[k-1]*k
    proto=np.zeros(N)
    # s2 doesn't depend on i
    for i in range(N):
        n=i-half
        h=two_fc if n==0 else math.sin(twopi_fc*n)/(n*3.141592653589793)
        x=i*step; xm1=x-1.0
        t=fma_d(x,xm1,-xm1) if fma_t else (x*xm1-xm1)
        arg=math.sqrt(1.0-t)*hb
        s1=0.0;s2=0.0;kd=0.0
        for k in range(22):
            f=fact[k]*fact[k]; e=kd+kd; rcp=1.0/f
            if fma_s:
                s1=fma_d(rcp,math.pow(arg,e),s1); s2=fma_d(rcp,math.pow(hb,e),s2)
            else:
                s1=rcp*math.pow(arg,e)+s1; s2=rcp*math.pow(hb,e)+s2
            kd+=1.0
        proto[i]=(s1*h)/s2
    idx=[(M*p)//L for p in range(L)]
    tab=np.zeros((L,TAPS),dtype=np.float32)
    for p in range(L):
        for j in range(TAPS):
            k=L*(idx[p]+j+1)-M*p-1
            tab[p,j]=np.float32(proto[k]*L)
    return tab
f32=np.float32
def fmaf_v(a,b,c): return (a.astype(np.float64)*b.astype(np.float64)+c.astype(np.float64)).astype(f32)
def mad(a,b,c): return (a*b).astype(f32)+c
def conv(tab,X,q,p,kind):
    T=lambda j: tab[p,j]; S=lambda j: X[q+j]
    z=np.zeros(len(q),f32)
    if kind=='fma_pair':
        al=z;a=0;b=28
        while a<b: al=fmaf_v(T(a),S(a),al); al=fmaf_v(T(b),S(b),al); a+=1;b-=1
        return fmaf_v(T(a),S(a),al)
    if kind=='mul_pair':
        al=z;a=0;b=28
        while a<b: al=mad(T(a),S(a),al); al=mad(T(b),S(b),al); a+=1;b-=1
        return mad(T(a),S(a),al)
    if kind=='mul_seq':
        al=z
        for j in range(29): al=mad(T(j),S(j),al)
        return al
    if kind=='mul_rev':
        al=z
        for j in range(28,-1,-1): al=mad(T(j),S(j),al)
        return al
    if kind=='fma_seq':
        al=z
        for j in range(29): al=fmaf_v(T(j),S(j),al)
        return al
    if kind=='fma_rev':
        al=z
        for j in range(28,-1,-1): al=fmaf_v(T(j),S(j),al)
        return al
    if kind=='mul_pairsum':   # (h[a]x[a] + h[b]x[b]) added to acc
        al=z;a=0;b=28
        while a<b: al=al+((T(a)*S(a)).astype(f32)+(T(b)*S(b)).astype(f32)); a+=1;b-=1
        return mad(T(a),S(a),al)
    if kind=='mul_2acc':      # front and back accumulators
        u=z;v=z;a=0;b=28
        while a<b: u=mad(T(a),S(a),u); v=mad(T(b),S(b),v); a+=1;b-=1
        return mad(T(a),S(a),u+v)
    if kind=='mul_2acc_b':
        u=z;v=z;a=0;b=28
        while a<b: u=mad(T(a),S(a),u); v=mad(T(b),S(b),v); a+=1;b-=1
        return (mad(T(a),S(a),u))+v
    if kind=='mul_evenodd':
        u=z;v=z
        for j in range(0,28,2): u=mad(T(j),S(j),u); v=mad(T(j+1),S(j+1),v)
        return mad(T(28),S(28),u+v)
    if kind=='mul_4acc':
        acc=[z,z,z,z]
        for j in range(28): acc[j%4]=mad(T(j),S(j),acc[j%4])
        return mad(T(28),S(28),(acc[0]+acc[1])+(acc[2]+acc[3]))
    if kind=='dbl_seq':
        al=np.zeros(len(q))
        for j in range(29): al=al+T(j).astype(np.float64)*S(j).astype(np.float64)
        return al.astype(f32)
    if kind=='dbl_pair':
        al=np.zeros(len(q));a=0;b=28
        while a<b: al=al+T(a).astype(np.float64)*S(a).astype(np.float64); al=al+T(b).astype(np.float64)*S(b).astype(np.float64); a+=1;b-=1
        return (al+T(a).astype(np.float64)*S(a).astype(np.float64)).astype(f32)
def load():
    f=r"C:\music\DJMusic\Lauren\YACHT\Psychic City\Psychic City - Classixx Remix.flac"
    r=subprocess.run(["ffmpeg","-v","error","-i",f,"-f","f32le","-ac","2","-ar","48000","-"],capture_output=True)
    d=np.frombuffer(r.stdout,dtype=np.float32).reshape(-1,2)
    dumps=sorted(glob.glob(r"C:\rbstems-dump\*.mix"))
    ref=np.fromfile(dumps[38],dtype=np.float32)
    return d,ref
if __name__=="__main__":
    d,ref=load()
    x=np.ascontiguousarray(d[:,0]); NN=300000
    refL=ref[1024:1024+NN]
    L=882;M=960;fc=0.52/M
    X=np.concatenate([np.zeros(HIST,f32),x,np.zeros(TAPS,f32)])
    n=np.arange(0,NN,dtype=np.int64); q=(M*n)//L; p=n%L
    for ft,fs in itertools.product([True,False],[True,False]):
        tab=build(L,M,fc,8,ft,fs)
        for kind in ['fma_pair','mul_pair','mul_seq','mul_rev','fma_seq','fma_rev','mul_pairsum','mul_2acc','mul_2acc_b','mul_evenodd','mul_4acc','dbl_seq','dbl_pair']:
            y=conv(tab,X,q,p,kind)
            print(ft,fs,kind,int((y!=refL).sum()),float(np.abs(y-refL).max()),flush=True)
