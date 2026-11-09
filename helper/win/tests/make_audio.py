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
