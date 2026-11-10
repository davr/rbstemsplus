import numpy as np, sys, os, itertools
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from var import *
d,ref=load()
x=np.ascontiguousarray(d[:,0]); NN=150000
refL=ref[1024:1024+NN]
L=882;M=960
X=np.concatenate([np.zeros(HIST,f32),x,np.zeros(TAPS,f32)])
n=np.arange(0,NN,dtype=np.int64); q=(M*n)//L; p=n%L
tab=np.load('tab48.npy')
P=[(tab[p,j].astype(np.float64),X[q+j].astype(np.float64)) for j in range(29)]
D=np.float64
def fma(j,c): h,s=P[j]; return (h*s+c.astype(D)).astype(f32)
def mul(j): h,s=P[j]; return (h*s).astype(f32)
z=np.zeros(NN,f32)
def rep(name,y): print(name,int((y!=refL).sum()),flush=True)
for midf in (0,1):
    al=z
    for a in range(14): al=fma(a,al); al=al+mul(28-a)
    rep(f'a fused, b mul+add, mid fused={midf}', fma(14,al) if midf else al+mul(14))
    al=z
    for a in range(14): al=al+mul(a); al=fma(28-a,al)
    rep(f'a mul+add, b fused, mid fused={midf}', fma(14,al) if midf else al+mul(14))
    al=z
    for a in range(14): al=al+fma(a,mul(28-a))
    rep(f'acc += fma(a, mul(b)), mid fused={midf}', fma(14,al) if midf else al+mul(14))
    al=z
    for a in range(14): al=al+fma(28-a,mul(a))
    rep(f'acc += fma(b, mul(a)), mid fused={midf}', fma(14,al) if midf else al+mul(14))
    al=z
    for a in range(14): al=fma(a,al+mul(28-a))
    rep(f'acc = fma(a, acc+mul(b)), mid fused={midf}', fma(14,al) if midf else al+mul(14))
    al=z
    for a in range(14): al=fma(28-a,al+mul(a))
    rep(f'acc = fma(b, acc+mul(a)), mid fused={midf}', fma(14,al) if midf else al+mul(14))
