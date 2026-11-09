import sys,array,glob
L=485100;HOP=483052;PRE=1024
raw,fmt,first,cnt=sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4])
if fmt=="s16":
    a=array.array('h'); a.frombytes(open(raw,'rb').read()); x=[v/32768.0 for v in a]
else:
    a=array.array('f'); a.frombytes(open(raw,'rb').read()); x=list(a)
n=len(x)//2
print("samples",n)
dump=sorted(glob.glob(r"C:\rbstems-dump\*.mix"))[first:first+cnt]
for i,p in enumerate(dump):
    ref=array.array('f'); ref.frombytes(open(p,'rb').read())
    lo=i*HOP-PRE
    for off in ([0] if i else [0,1,-1,2,-2,1105,-1105]):
        md=0.0;cnt0=0
        for j in range(L):
            k=lo+j+off
            v=x[2*k] if 0<=k<n else 0.0
            if k==n-1: v=0.0
            d=abs(v-ref[j]); 
            if d>md: md=d
            if d!=0: cnt0+=1
        print(f"chunk {i} off {off}: max|diff|={md:.3e} nonequal={cnt0}")
        if md==0: break
