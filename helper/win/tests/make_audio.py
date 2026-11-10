"""make_audio.py DIR: deterministic test audio, 16-bit stereo 44.1 kHz, 11 s (two chunks): a.wav, a.aiff, and
a.expected.json with the keys the helper must write for a.wav for a model with sha256 MODEL (argv[2])."""
import hashlib, json, math, random, struct, sys, wave
d, model = sys.argv[1], sys.argv[2]
rate, secs = 44100, 11
n = rate * secs
rnd = random.Random(7)
frames = []
for i in range(n):
    t = i / rate
    l = int(9000 * math.sin(2 * math.pi * 220 * t) + rnd.randint(-2000, 2000))
    r = int(7000 * math.sin(2 * math.pi * 330 * t) + rnd.randint(-2000, 2000))
    frames.append((l, r))
with wave.open(d + "/a.wav", "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(b"".join(struct.pack("<hh", *f) for f in frames))
# AIFF, 16-bit big-endian PCM
def ext80(x):
    m, e = math.frexp(x)
    return struct.pack(">HQ", e + 16382, int(m * (1 << 64)))
data = b"".join(struct.pack(">hh", *f) for f in frames)
comm = struct.pack(">hIh", 2, n, 16) + ext80(rate)
body = b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm + b"SSND" + struct.pack(">I", len(data) + 8) + struct.pack(">II", 0, 0) + data
open(d + "/a.aiff", "wb").write(b"FORM" + struct.pack(">I", len(body)) + body)
# the keys rekordbox's chunking gives (helper/rbsp.h): chunk i starts at i*483052-1024, 485100 samples, the track's last sample never sent
L, HOP, PRE = 485100, 483052, 1024
def chunk(i):
    out = []
    for ch in (0, 1):
        o = [0.0] * L
        lo = i * HOP - PRE
        for j in range(max(lo, 0), min(lo + L, n)):
            o[j - lo] = frames[j][ch] / 32768.0
        if lo <= n - 1 < lo + L:
            o[n - 1 - lo] = 0.0
        out.append(o)
    return out
keys = []
for i in range((n + PRE + HOP - 1) // HOP):
    c = chunk(i)
    h = hashlib.sha256()
    h.update(b"rbstems-v1\0" + model.encode() + struct.pack("<3q", 1, 2, L) + struct.pack("<4q", 1, 4, 2048, 474))
    for ch in c:
        h.update(struct.pack("<%df" % L, *ch))
    keys.append(h.hexdigest())
json.dump({"keys": keys}, open(d + "/a.expected.json", "w"))

# ---------------------------------------------------------------- 48 kHz: b48.wav through rekordbox's resampler
# An independent implementation of rekordbox's x64 resampler (helper/resample.c with build.sh's one change): the
# Kaiser table in double, then per output sample 29 taps from both ends, the first of each pair a fused
# multiply-add, the second a multiply then an add, the middle tap fused. Every float32 step is rounded exactly.
from fractions import Fraction
def f32(x):                      # a double, rounded to float32 (x is exactly a double here)
    return struct.unpack("<f", struct.pack("<f", x))[0]
def round32(q):                  # a Fraction, rounded once to the nearest float32 (ties to even)
    if q == 0: return 0.0
    c = f32(float(q))            # float(q) is the nearest double; c is within one float32 step of the answer
    step = abs(c) * 2.0 ** -23 or 2.0 ** -149
    best = None
    for v in (c, f32(c - step), f32(c + step), f32(c - step / 2), f32(c + step / 2)):
        d = abs(Fraction(v) - q)
        even = struct.unpack("<I", struct.pack("<f", v))[0] & 1 == 0
        if best is None or d < best[0] or (d == best[0] and even and not best[2]): best = (d, v, even)
    return best[1]
def fma_d(a, b, c): return float(Fraction(a) * Fraction(b) + Fraction(c))
def table(L, M, fc, beta):
    N = 29 * L; half = N // 2
    twopi_fc = fc * 6.283185307179586; two_fc = fc + fc; inv = 1.0 / (N - 1); step = inv + inv; hb = beta * 0.5
    fact = [1.0] * 22
    for k in range(2, 22): fact[k] = fact[k - 1] * k
    proto = []
    for i in range(N):
        nn = i - half
        h = two_fc if nn == 0 else math.sin(twopi_fc * nn) / (nn * 3.141592653589793)
        x = i * step; xm1 = x + -1.0
        arg = math.sqrt(1.0 - fma_d(x, xm1, -xm1)) * hb
        s1 = s2 = 0.0
        for k in range(22):
            rcp = 1.0 / (fact[k] * fact[k])
            s1 = fma_d(rcp, math.pow(arg, 2.0 * k), s1); s2 = fma_d(rcp, math.pow(hb, 2.0 * k), s2)
        proto.append((s1 * h) / s2)
    return [[f32(proto[L * ((M * p) // L + j + 1) - M * p - 1] * L) for j in range(29)] for p in range(L)]
rate48, n48 = 48000, 4800        # a tenth of a second: exact arithmetic in Python is slow
rnd = random.Random(11)
fr48 = [(int(8000 * math.sin(2 * math.pi * 440 * i / rate48) + rnd.randint(-3000, 3000)), int(6000 * math.sin(2 * math.pi * 550 * i / rate48) + rnd.randint(-3000, 3000))) for i in range(n48)]
with wave.open(d + "/b48.wav", "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate48)
    w.writeframes(b"".join(struct.pack("<hh", *f) for f in fr48))
Lp, Mp = 44100 // 50, rate48 // 50
tab = table(Lp, Mp, 0.52 / Mp, 8)
n_out = ((n48 - 1) * Lp) // Mp + 1
while n_out > 0 and (Mp * (n_out - 1)) // Lp > n48 - 1: n_out -= 1
while (Mp * n_out) // Lp <= n48 - 1: n_out += 1
res = []
for ch in (0, 1):
    X = [0.0] * 28 + [f[ch] / 32768.0 for f in fr48] + [0.0] * 29
    o = []
    for i in range(n_out):
        q = (Mp * i) // Lp; h = tab[i % Lp]; acc = 0.0
        for a in range(14):
            acc = round32(Fraction(h[a]) * Fraction(X[q + a]) + Fraction(acc))
            acc = f32(acc + f32(h[28 - a] * X[q + 28 - a]))
        o.append(round32(Fraction(h[14]) * Fraction(X[q + 14]) + Fraction(acc)))
    res.append(o)
hsh = hashlib.sha256()
hsh.update(b"rbstems-v1\0" + model.encode() + struct.pack("<3q", 1, 2, L) + struct.pack("<4q", 1, 4, 2048, 474))
for o in res:
    c = [0.0] * L
    for j in range(n_out): c[j + PRE] = o[j]
    c[n_out - 1 + PRE] = 0.0     # the track's last sample is never sent
    hsh.update(struct.pack("<%df" % L, *c))
json.dump({"keys": [hsh.hexdigest()], "samples": n_out}, open(d + "/b48.expected.json", "w"))
