#!/usr/bin/env python3
"""Look inside Canon CPCA / UFR II LT print jobs (what the MF3010 driver sends).

  ufrdump.py packets FILE        list the CPCA packets
  ufrdump.py pdl FILE            dump the page description commands
  ufrdump.py bands FILE OUTDIR   save each page as a .jbg file (jbgtopbm can open it)
  ufrdump.py jobinfo FILE        job name, user, host etc. as shell variables

To capture a job on Windows: printer properties > Ports > tick "FILE:",
print something, and give it a file name.
"""
import os
import shlex
import struct
import sys


def packets(b):
    i = 0
    while i < len(b):
        if b[i:i + 2] != b"\xcd\xca":
            raise ValueError(f"bad magic at {i:#x}: {b[i:i + 8].hex()}")
        ver, kind, op, one, ln, ff = struct.unpack(">BBHHHH", b[i + 2:i + 12])
        pad = b[i + 12:i + 20]
        yield i, ver, kind, op, one, ln, ff, pad, b[i + 20:i + 20 + ln]
        i += 20 + ln


def pdl_stream(b):
    out = bytearray()
    for off, ver, kind, op, one, ln, ff, pad, pl in packets(b):
        if kind == 2:
            if pl[:1] != b"\x01":
                raise ValueError(f"data packet without 01 lead byte at {off:#x}")
            out += pl[1:]
    return bytes(out)


def tokens(s):
    i = 0
    while i < len(s):
        c = s[i]
        if c in (0x9C, 0x9D, 0x9E):
            n = {0x9C: 1, 0x9D: 2, 0x9E: 4}[c]
            ln = int.from_bytes(s[i + 1:i + 1 + n], "big")
            yield ("data", i, s[i + 1 + n:i + 1 + n + ln])
            i += 1 + n + ln
        elif c >= 0xC0:
            t = s[i + 1]
            if t < 0x80:
                yield ("attr", i, c, t, None)
                i += 2
            else:
                esize = 1 << ((t >> 2) & 3)
                count = (t & 3) + 1
                raw = s[i + 2:i + 2 + esize * count]
                signed = bool(t & 0x20)
                vals = tuple(int.from_bytes(raw[k * esize:(k + 1) * esize], "big", signed=signed)
                             for k in range(count))
                yield ("attr", i, c, vals if count > 1 else vals[0], t)
                i += 2 + esize * count
        elif c < 0x80:
            yield ("op", i, c)
            i += 1
        else:
            raise ValueError(f"unexpected byte {c:#x} at {i}")


def cmd_packets(path):
    b = open(path, "rb").read()
    for off, ver, kind, op, one, ln, ff, pad, pl in packets(b):
        extra = "" if (one == 1 and ff == 0xFFFF and pad == bytes(8)) else \
            f" one={one:#x} ff={ff:#x} pad={pad.hex()}"
        shown = pl[:48].hex(" ")
        more = " ..." if ln > 48 else ""
        print(f"{off:6x} v{ver:02x} k{kind:02x} op={op:#06x} len={ln:5d}{extra}  {shown}{more}")


def cmd_pdl(path):
    s = pdl_stream(open(path, "rb").read())
    for tok in tokens(s):
        if tok[0] == "op":
            print(f"\n{tok[1]:6d} OP {tok[2]:#04x}", end="")
        elif tok[0] == "attr":
            v = tok[3]
            vs = f"({', '.join(map(str, v))})" if isinstance(v, tuple) else str(v)
            ty = f"[t{tok[4]:02x}]" if tok[4] is not None else ""
            print(f"  {tok[2]:#04x}={vs}{ty}", end="")
        else:
            print(f"  DATA[{len(tok[2])}]", end="")
    print()


def cmd_bands(path, outdir):
    os.makedirs(outdir, exist_ok=True)
    s = pdl_stream(open(path, "rb").read())
    pages = []
    for tok in tokens(s):
        if tok[0] == "op" and tok[2] == 0x03:
            pages.append([])
        elif tok[0] == "data":
            pages[-1].append(tok[2])
    for n, bands in enumerate(pages, 1):
        bih = bands[0][:20]
        xd, yd, l0 = struct.unpack(">III", bih[4:16])
        body = bands[0][20:] + b"".join(bands[1:])
        name = os.path.join(outdir, f"page{n}.jbg")
        open(name, "wb").write(bih + body)
        print(f"page {n}: bands={len(bands)} BIH={bih.hex(' ')} XD={xd} YD={yd} L0={l0} -> {name}")
        for k, bd in enumerate(bands):
            print(f"   band {k}: {len(bd)} bytes, ends {bd[-2:].hex()}")


def cmd_jobinfo(path):
    b = open(path, "rb").read()
    first = next(packets(b))
    pl = first[-1]
    n = struct.unpack(">H", pl[:2])[0]
    i = 2
    tlv = {}
    for _ in range(n):
        t, ln = struct.unpack(">HH", pl[i:i + 4])
        tlv[t] = pl[i + 4:i + 4 + ln]
        i += 4 + ln

    def text(v):
        return v[3:3 + v[2]].decode("latin-1") if v else ""

    info = tlv.get(0x0118, b"")
    out = {
        "NAME": text(tlv.get(0x0004)),
        "USER_NAME": text(tlv.get(0x0006)),
        "HOST": info[32:48].split(b"\0")[0].decode("latin-1"),
        "JOB": str(int(info[16:32] or b"0")),
        "TIME": tlv.get(0x0009, bytes(8)).hex(),
    }
    for k, v in out.items():
        print(f"{k}={shlex.quote(v)}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "packets":
        cmd_packets(sys.argv[2])
    elif cmd == "pdl":
        cmd_pdl(sys.argv[2])
    elif cmd == "bands":
        cmd_bands(sys.argv[2], sys.argv[3])
    elif cmd == "jobinfo":
        cmd_jobinfo(sys.argv[2])
    else:
        sys.exit(__doc__)
