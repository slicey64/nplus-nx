#!/usr/bin/env python3
"""Build a loadable default.xex for N+ (XBLA) from its 12 KB XEX2 header and the decrypted basefile.

The N+ download carries data/Original/default.xex as the XEX2 header alone (header_size 0x3000 bytes, no image)
and data/Original/NPlus.exe as the already decrypted, decompressed memory image. The loader
(sdk/src/system/xex_module.cpp, ReadImageUncompressed) copies everything after header_size straight into memory
when the file format info says encryption NONE and compression NONE, so this sets those two fields and appends
the image. Nothing is decrypted here.

usage: nplus_make_xex.py <header default.xex> <NPlus.exe> <out default.xex>
"""
import struct
import sys

XEX_FILE_FORMAT_INFO = 0x000003FF


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    header = bytearray(open(sys.argv[1], "rb").read())
    image = open(sys.argv[2], "rb").read()

    if header[:4] != b"XEX2":
        sys.exit("not an XEX2 header")
    header_size, _, security_offset, opt_count = struct.unpack_from(">IIII", header, 8)
    if len(header) < header_size:
        sys.exit(f"header file is {len(header):#x} bytes, header_size says {header_size:#x}")
    header = header[:header_size]

    image_size = struct.unpack_from(">I", header, security_offset + 4)[0]
    if len(image) != image_size:
        sys.exit(f"image is {len(image):#x} bytes, the security info says {image_size:#x}")
    if image[:2] != b"MZ":
        sys.exit("image has no PE header: is it the decrypted basefile?")

    ffi = None
    for i in range(opt_count):
        key, value = struct.unpack_from(">II", header, 24 + 8 * i)
        if key == XEX_FILE_FORMAT_INFO:
            ffi = value
    if ffi is None:
        sys.exit("no file format info in the optional headers")
    encryption, compression = struct.unpack_from(">HH", header, ffi + 4)
    print(f"file format info at {ffi:#x}: encryption {encryption}, compression {compression} -> 0, 0")
    struct.pack_into(">HH", header, ffi + 4, 0, 0)

    with open(sys.argv[3], "wb") as f:
        f.write(header)
        f.write(image)
    print(f"wrote {sys.argv[3]}: header {header_size:#x} + image {len(image):#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
