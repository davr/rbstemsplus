import numpy as np, sys, os, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from var import *
d,_=load()
tab=np.load('tab48.npy'); L=882;M=960
dumps=sorted(glob.glob(r"C:\rbstems-dump\*.mix"))
CL=485100;HOP=483052;PRE=1024
nin=len(d)
n_out=((nin-1)*L)//M+1
while (M*(n_out-1))//L>nin-1: n_out-=1
while (M*n_out)//L<=nin-1: n_out+=1
print('nin',nin,'n_out',n_out,'chunks',-(-(n_out-1+PRE)//HOP) )
D=np.float64
def rs(x):
    X=np.concatenate([np.zeros(HIST,f32),x,np.zeros(TAPS,f32)])
    n=np.arange(0,n_out,dtype=np.int64); q=(M*n)//L; p=n%L
    al=np.zeros(n_out,f32)
    for a in range(14):
        al=(tab[p,a].astype(D)*X[q+a].astype(D)+al.astype(D)).astype(f32)
        al=al+(tab[p,28-a]*X[q+28-a]).astype(f32)
    return (tab[p,14].astype(D)*X[q+14].astype(D)+al.astype(D)).astype(f32)
yl=rs(np.ascontiguousarray(d[:,0])); yr=rs(np.ascontiguousarray(d[:,1]))
total=n_out-1     # the final sample is never sent
i=0;bad=0
while i*HOP-PRE<total:
    lo=i*HOP-PRE
    got=np.zeros((2,CL),f32)
    a=max(lo,0); b=min(lo+CL,total)
    got[0,a-lo:b-lo]=yl[a:b]; got[1,a-lo:b-lo]=yr[a:b]
    ref=np.fromfile(dumps[38+i],dtype=f32).reshape(2,CL)
    ne=int((got!=ref).sum()); bad+=ne
    print('chunk',i,'nonequal',ne)
    i+=1
print('chunks',i,'total nonequal',bad)
