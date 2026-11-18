# What the MF3010 driver actually sends

Notes from taking apart print jobs captured from the Windows driver
("Canon MF3010", UFR II LT). Most of the structure matches what
[pkhr/mf4700-print-scan](https://github.com/pkhr/mf4700-print-scan) figured out
for the MF4700 series, which saved me a lot of time. The MF3010 specific bits
(resolution, page size, the job header) are from my own captures.

To get a capture on Windows: printer properties > Ports > tick `FILE:` >
print something > type a file name. `tools/ufrdump.py` can then pick it apart:

```
python3 tools/ufrdump.py packets hello.prn
python3 tools/ufrdump.py pdl hello.prn
python3 tools/ufrdump.py bands hello.prn out/ && jbgtopbm out/page1.jbg page1.pbm
```

Two cleaned up captures are in `test/captures/` (saved as `.ufr`, Windows doesn't like `.prn` files in some places).

## Layer 1: CPCA packets

Everything is wrapped in packets with a 20 byte header:

```
CD CA 10 KK OO OO 00 01 LL LL FF FF 00 00 00 00 00 00 00 00   payload (LL LL bytes)
         |  '---'       '---'
         |  opcode      payload length (big endian)
         kind: 00 = control, 02 = page data
```

A one page job looks like this:

| op | what |
|----|------|
| `006B` | job start, a list of TLVs (see below) |
| `0012` | job attribute `07D7 0001` (copies) |
| `0014` | binder start |
| `0015` ... | binder attributes (name, copies `07D7`, ...) |
| `0017` | document start |
| `0018` ... | document attributes, incl. `003A 08 0258 0190` = 600 x 400 dpi and `07ED ... 03` = A4 |
| `001A` (kind 02) | page data: payload is `01` followed by UFR II LT bytes |
| `001A` (kind 00) | payload `01`, end of page data |
| `0019` | document end |
| `0015` | `0113` + page count |
| `0016` | binder end |
| `0012` | `0113` + page count, then `07D6` + total job size in bytes |
| `0013` | job end |

Copies are done by the printer: set `07D7` in the binder to the number of copies.

### Job start TLVs

`count(2)` then `type(2) length(2) value` entries:

| type | value |
|------|-------|
| `00F0` | `01` |
| `0130` | `03 0000` |
| `0004` | job name (`00 04` + length byte + text, ISO-8859-1) |
| `0117` | document name, same format |
| `0006` | user name, same format (left out if empty) |
| `000D` | `04` |
| `0118` | 16 byte driver id, 16 digit job counter, computer name twice (16 bytes each, zero padded) |
| `000C` | `32` |
| `0009` | time, see below |

The time field is 8 bytes: `u32(year << 20 | month << 16 | day << 11 | weekday << 8)`,
hour, minute, `u16(seconds * 1000 + ms)`, in UTC.

## Layer 2: UFR II LT

The page data is a byte stream of commands. Each command is one opcode byte
followed by attributes (attribute id byte + a type byte that says how big the
value is). `ufrdump.py pdl` decodes it. For the MF3010:

```
01  c1=(4096,4228) c2=0 d8=600 dd=200 f0=2048 e9=400     session: 600 x 400 dpi
02  c3=0 c4=0 c5=0 c6=0
51  f2=0
03  e7=(4720,6774) de=0 c8=0 ca=(120,120) cb=0           page start
61  e6=1 e5=0                                              (first band only)
62  e3=(4720,256) e8=(0,y) e1=5 d7=<len> <JBIG data>       one band
...
13                                                         page end
12 11                                                      paper group / session end
```

* The page bitmap is **4720 x 4516 pixels at 600 x 400 dpi**, 1 bit per pixel.
  Yes, the vertical resolution is lower. That's the sheet (A4 at 600 dpi is
  4960 x 7016) minus 120 px (5.08 mm) on every side, with the height scaled to 400 dpi.
  The page size in `03` is given in 600 dpi units (6774 = 4516 x 1.5).
* The page is sent in bands of 256 lines, the last one is shorter (164 lines).
* More pages just repeat `03 61 62... 13` before the final `12 11`.
* Compared to the MF4700 notes, the session has an extra `e9=400` (the vertical
  resolution) and `c1` ends in `1084` instead of `1081`.

## Layer 3: JBIG

Every band is its own JBIG1 (T.82) image:

* 1 plane, L0 = 128 lines per stripe (so 2 stripes per band), MX = MY = 0
* options LRLTWO + TPBON (two line template, typical prediction)
* the first band of a page starts with the normal 20 byte JBIG header (BIH), with
  YD = the full page height. The other bands have no header.
* stripes end with `FF 02` (SDNORM), except the last stripe of each band, which
  ends with `FF 03` (SDRST). That resets the coder so the next band starts fresh.

So if you glue the first band's BIH and all band data together, you get one
normal JBIG file of the whole page, and `jbgtopbm` from JBIG-KIT opens it. That's
what `ufrdump.py bands` does.

JBIG-KIT can encode this too, but it wants the whole band in memory
(4720 x 256 / 8 = 151 KB). `firmware/MF3010_bridge/jbig_stream.cpp` does the same
thing line by line with ~6 KB, and `make check` compares it against JBIG-KIT and
rebuilds both captures byte for byte.
