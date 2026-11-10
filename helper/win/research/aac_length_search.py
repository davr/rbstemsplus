import numpy as np, hashlib, struct, os, sys
raw=np.fromfile(r'C:\dj\rs\mfp\om.raw',dtype='<i2').reshape(-1,2)
N=len(raw); print('decoded',N)
model='0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772'
cache=os.path.expandvars(r'%LOCALAPPDATA%\rbstemsplus\Cache')+'/'+model
L=485100;HOP=483052;PRE=1024
pre=b"rbstems-v1\0"+model.encode()+struct.pack("<3q",1,2,L)+struct.pack("<4q",1,4,2048,474)
x=(raw.astype(np.float32)/np.float32(32768.0))
def key(i,n):
    lo=i*HOP-PRE
    c=np.zeros((2,L),np.float32)
    a=max(lo,0); b=min(lo+L,n-1)
    if b>a: c[:,a-lo:b-lo]=x[a:b].T
    return hashlib.sha256(pre+c.tobytes()).hexdigest()
def have(k): return os.path.exists(cache+'/'+k[:2]+'/'+k+'.flac')
print('chunk0 in cache',have(key(0,N)),' chunk29',have(key(29,N)), 'chunk30(ours)',have(key(30,N)))
target='9851d36394499cde6f79f8d94fdddf2922115fb59c5792ed6b6f9962676dcded'
# scan
lo=30*HOP-PRE
base=np.zeros((2,L),np.float32)
for n in list(range(N,N-4000,-1)):
    if key(30,n)==target: print('FOUND n =',n,'= decoded -',N-n); break
else: print('not found in last 4000')
