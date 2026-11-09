import sys,os,glob,subprocess,struct,array,math
L=485100;HOP=483052;PRE=1024
def load_f32(path):
    a=array.array('f'); a.frombytes(open(path,'rb').read()); return a
def decode(path,extra=()):
    r=subprocess.run(["ffmpeg","-v","error","-i",path,"-f","f32le","-ac","2","-ar","44100",*extra,"-"],capture_output=True)
    a=array.array('f'); a.frombytes(r.stdout); return a   # interleaved
dump=sorted(glob.glob(r"C:\rbstems-dump\*.mix"))
path=sys.argv[1]; n_first=int(sys.argv[2]); n_last=int(sys.argv[3]) if len(sys.argv)>3 else None
mixes=[load_f32(p) for p in dump[n_first:n_last]]
d=decode(path)
n=len(d)//2
print("ffmpeg decode:",n,"samples", round(n/44100,2),"s;  dump chunks:",len(mixes))
def chunk(i,off=0):
    lo=i*HOP-PRE+off
    c=[array.array('f',[0.0])*L,array.array('f',[0.0])*L]
    for ch in range(2):
        for j in range(max(lo,0),min(lo+L,n)):
            c[ch][j-lo]=d[2*j+ch]
    return c
for i,m in enumerate(mixes[:4]):
    best=None
    for off in range(-3000,3001) if i==0 else [best_off]:
        pass
    # find offset by correlating a window of the dumped left channel
    ref=m[:L]
    if i==0:
        # dumped chunk 0 starts at -PRE: zeros for first 1024, then sample 0 (if no delay)
        # locate first nonzero index
        nz=next((k for k,v in enumerate(ref) if abs(v)>1e-9),None)
        print("chunk0 first nonzero idx",nz)
        # search the best integer offset of ffmpeg decode relative to dump (+- 3000) using a middle window
        w0=200000; win=ref[w0:w0+2000]
        bestv=1e30; best_off=0
        for off in range(-3000,3001):
            s=w0-PRE+off
            if s<0 or s+2000>n: continue
            e=sum((win[k]-d[2*(s+k)])**2 for k in range(0,2000,7))
            if e<bestv: bestv=e; best_off=off
        print("best offset (samples) of ffmpeg decode vs rekordbox chunk0:",best_off,"err",bestv)
    c=chunk(i,best_off)
    md=max(abs(a-b) for a,b in zip(ref,c[0])); ms=sum((a-b)**2 for a,b in zip(ref,c[0]))/L
    print(f"chunk {i}: max|diff| left = {md:.3e}  mse = {ms:.3e}")
