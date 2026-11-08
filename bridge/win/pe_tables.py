"""Prints a PE file's machine, exports and imports, for bridge/win/build.sh's checks (objdump's
text output differs between binutils versions; this reads the tables themselves):

    machine x64
    export <ordinal> <name>
    import <dll name, lower case>
"""
import struct
import sys

d = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
assert d[pe:pe + 4] == b"PE\0\0", "not a PE file"
machine, nsec = struct.unpack_from("<HH", d, pe + 4)
optsize = struct.unpack_from("<H", d, pe + 20)[0]
opt = pe + 24
magic = struct.unpack_from("<H", d, opt)[0]
dd = opt + (112 if magic == 0x20B else 96)
dirs = [struct.unpack_from("<II", d, dd + 8 * i) for i in range(16)]
secs = [struct.unpack_from("<IIII", d, opt + optsize + 40 * i + 8) for i in range(nsec)]   # vsize, va, rawsize, rawptr


def off(rva):
    for vsize, va, rawsize, rawptr in secs:
        if va <= rva < va + max(vsize, rawsize):
            return rva - va + rawptr
    raise ValueError("rva %x" % rva)


def cstr(o):
    return d[o:d.index(b"\0", o)].decode("ascii")


print("machine", {0x8664: "x64", 0x14C: "x86", 0xAA64: "arm64"}.get(machine, hex(machine)))
erva = dirs[0][0]
if erva:
    e = off(erva)
    base, nfun, nname, afun, aname, aord = struct.unpack_from("<IIIIII", d, e + 16)
    for i in range(nname):
        name = cstr(off(struct.unpack_from("<I", d, off(aname) + 4 * i)[0]))
        print("export", base + struct.unpack_from("<H", d, off(aord) + 2 * i)[0], name)
irva = dirs[1][0]
if irva:
    o = off(irva)
    while True:
        oft, ts, fc, name, ft = struct.unpack_from("<IIIII", d, o)
        if not name and not ft:
            break
        print("import", cstr(off(name)).lower())
        o += 20
