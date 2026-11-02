#!/usr/bin/env python3
"""
pg5tool.py - Python replacement for the DMCP5 SDK helper scripts
(bin/check_qspi_crc and bin/add_pgm_chsum), so the build needs no bash/crc32/tac.

  pg5tool.py qspi  <qspi.bin> <qspi_crc.h>   update header; exit 1 if it changed
  pg5tool.py pg5   <flash.bin> <out.pg5>     patch size word + append SHA1

PG5 layout (same as add_pgm_chsum): bytes 0..3 of flash.bin, then the
file size as little-endian uint32, then flash.bin from byte 8, then the raw
20-byte SHA1 of everything before it.
"""
import hashlib
import os
import re
import struct
import sys
import zlib


def qspi(qbin, header):
    data = open(qbin, "rb").read() if os.path.exists(qbin) else b""
    nsz, ncrc = len(data), zlib.crc32(data) & 0xFFFFFFFF
    osz, ocrc = None, None
    if os.path.exists(header):
        txt = open(header).read()
        m1 = re.search(r"QSPI_DATA_SIZE\s+(\S+)", txt)
        m2 = re.search(r"QSPI_DATA_CRC\s+(\S+)", txt)
        if m1 and m2:
            try:
                osz, ocrc = int(m1.group(1), 0), int(m2.group(1), 0)
            except ValueError:
                pass
    if (osz, ocrc) != (nsz, ncrc):
        with open(header, "w") as f:
            f.write("\n#define QSPI_DATA_SIZE %d\n#define QSPI_DATA_CRC  0x%08x\n\n" % (nsz, ncrc))
        print("QSPI contents changed (size %d, crc 0x%08x) - header updated, rebuilding." % (nsz, ncrc))
        return 1
    return 0


def pg5(fbin, out):
    data = bytearray(open(fbin, "rb").read())
    body = bytes(data[0:4]) + struct.pack("<I", len(data)) + bytes(data[8:])
    sha = hashlib.sha1(body).digest()
    with open(out, "wb") as f:
        f.write(body + sha)
    print("SHA1: %s  ->  %s (%d bytes)" % (sha.hex(), out, len(body) + 20))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("qspi", "pg5"):
        print(__doc__)
        sys.exit(2)
    fn = qspi if sys.argv[1] == "qspi" else pg5
    sys.exit(fn(sys.argv[2], sys.argv[3]))
