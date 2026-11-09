import numpy as np, math, sys, glob, array, subprocess
from fractions import Fraction
def fma_d(a,b,c):   # exact double fma via Fractions
    return float(Fraction(a)*Fraction(b)+Fraction(c))
TAPS=29;HIST=28
def params(i,o):
    t={(44100,48000):(0.54,8),(44100,96000):(0.58,10),(48000,44100):(0.52,8),(48000,96000):(0.45,10),(96000,44100):(0.55,6),(96000,48000):(0.47,10),(88200,44100):(0.55,6),(44100,88200):(0.58,10),(44100,176400):(0.48,9),(44100,192000):(0.48,9)}
    return t.get((i,o),(0.45,12))
def build(L,M,fc,beta):
    N=29*L; half=N//2
    twopi_fc=fc*6.283185307179586; two_fc=fc+fc; inv=1.0/(N-1); step=inv+inv; hb=beta*0.5
    fact=[1.0]*22
    for k in range(2,22): fact[k]=fact[k-1]*k
    proto=np.zeros(N)
    for i in range(N):
        n=i-half
        h=two_fc if n==0 else math.sin(twopi_fc*n)/(n*3.141592653589793)
        x=i*step; xm1=x-1.0
        t=fma_d(x,xm1,-xm1)
        arg=math.sqrt(1.0-t)*hb
        s1=0.0;s2=0.0;kd=0.0
        for k in range(22):
            f=fact[k]*fact[k]; e=kd+kd; rcp=1.0/f
            s1=fma_d(rcp,math.pow(arg,e),s1); s2=fma_d(rcp,math.pow(hb,e),s2); kd+=1.0
        proto[i]=(s1*h)/s2
    idx=[(M*p)//L for p in range(L)]
    tab=np.zeros((L,TAPS),dtype=np.float32)
    for p in range(L):
        for j in range(TAPS):
            k=L*(idx[p]+j+1)-M*p-1
            tab[p,j]=np.float32(proto[k]*L)
    return tab
def fmaf_v(a,b,c):  # float32 vectors a,b,c -> float32(a*b+c) with one rounding (via float64)
    return (a.astype(np.float64)*b.astype(np.float64)+c.astype(np.float64)).astype(np.float32)
def resample_range(x,rate,n0,n1):
    # x: float32 array of the input samples (one channel); output samples n0..n1
    to=44100; L=to//50; M=rate//50
    cutoff,beta=params(rate,to); fc=cutoff/(L if rate<to else M)
    tab=build(L,M,fc,beta)
    X=np.concatenate([np.zeros(HIST,np.float32),x,np.zeros(TAPS,np.float32)])
    n=np.arange(n0,n1,dtype=np.int64)
    q=(M*n)//L; p=(n%L)
    al=np.zeros(len(n),np.float32)
    a=0;b=TAPS-1
    while a<b:
        al=fmaf_v(tab[p,a],X[q+a],al); al=fmaf_v(tab[p,b],X[q+b],al); a+=1;b-=1
    al=fmaf_v(tab[p,a],X[q+a],al)
    return al
if __name__=="__main__":
    path=sys.argv[1]; rate=int(sys.argv[2]); first=int(sys.argv[3]); cnt=int(sys.argv[4])
    r=subprocess.run(["ffmpeg","-v","error","-i",path,"-f","f32le","-ac","2","-ar",str(rate),"-"],capture_output=True)
    d=np.frombuffer(r.stdout,dtype=np.float32).reshape(-1,2)
    x=np.ascontiguousarray(d[:,0]); print("input",len(x),"samples at",rate)
    L=485100;HOP=483052;PRE=1024
    dump=sorted(glob.glob(r"C:\rbstems-dump\*.mix"))[first:first+cnt]
    for i,p in enumerate(dump):
        ref=np.fromfile(p,dtype=np.float32)[:L]
        lo=i*HOP-PRE
        n0=max(lo,0); n1=lo+L
        y=resample_range(x,rate,n0,n1)
        got=np.zeros(L,np.float32); got[n0-lo:]=y
        diff=np.abs(got-ref)
        print(f"chunk {i}: max|diff|={diff.max():.3e} nonequal={(diff!=0).sum()}")
