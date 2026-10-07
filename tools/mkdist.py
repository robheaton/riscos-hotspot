#!/usr/bin/env python3
"""
mkdist.py - zips the built application the way zip tools on RISC OS do.

    mkdist.py OUTPUT.zip [README]

Names in the zip are plain ("!Run", not "!Run,feb"). What RISC OS needs to
know about each file - its file type, load and exec addresses, attributes -
travels in Acorn's "ARC0" extra field (header id 0x4341), which SparkFS and
Info-ZIP on RISC OS write and read, so any RISC OS unzip puts the file types
back. The layout copies a zip made on RISC OS itself (system 13, 20 byte ARC0
fields, stored directory entries, time stamps in centiseconds since 1900).

Files in the build directory carry their type as a ",xxx" suffix (the way an
SMB share shows it); one without a suffix is taken to be Text. The optional
README is put beside the application folder, at the top of the zip.

Python's zipfile only, no zip(1).
"""

import os
import struct
import sys
import time
import zipfile

ARC0_TAG = 0x4341           # "AC"
RISCOS_SYSTEM = 13          # "Acorn RISC OS" in the zip format
ATTR_RW_R = 0x13            # owner read+write, public read
EPOCH_1900_TO_1970 = 2208988800     # seconds


def split_type(name):
    """'!Run,feb' -> ('!Run', 0xFEB); no suffix -> Text."""
    if len(name) > 4 and name[-4] == "," and \
            all(c in "0123456789abcdefABCDEF" for c in name[-3:]):
        return name[:-4], int(name[-3:], 16)
    return name, 0xFFF


def arc0(filetype, ro_time, attr):
    """The extra field: load/exec encode the type and the time stamp."""
    load = 0xFFF00000 | (filetype << 8) | (ro_time >> 32)
    exec_ = ro_time & 0xFFFFFFFF
    return struct.pack("<HH4sIII4x", ARC0_TAG, 20, b"ARC0", load, exec_, attr)


def entry(name, filetype, is_dir, now):
    ro_time = int((now + EPOCH_1900_TO_1970) * 100)     # UTC, as RISC OS has it
    stamp = time.localtime(now)                         # DOS time: local
    info = zipfile.ZipInfo(name, (stamp.tm_year, stamp.tm_mon, stamp.tm_mday,
                                  stamp.tm_hour, stamp.tm_min,
                                  stamp.tm_sec & ~1))
    info.create_system = RISCOS_SYSTEM
    info.extra = arc0(filetype, ro_time, ATTR_RW_R)
    if is_dir:
        info.compress_type = zipfile.ZIP_STORED
        info.external_attr = 0x41ED0010
    else:
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0x81A40000
    return info


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: mkdist.py OUTPUT.zip [README]")

    out = sys.argv[1]
    readme = sys.argv[2] if len(sys.argv) == 3 else None
    src = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", "build", "riscos", "!Hotspot"))
    now = time.time()

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        if readme is not None:
            name, filetype = split_type(os.path.basename(readme))
            with open(readme, "rb") as f:
                z.writestr(entry(name, filetype, False, now), f.read())

        z.writestr(entry("!Hotspot/", 0xFFD, True, now), b"")
        for fname in sorted(os.listdir(src)):
            name, filetype = split_type(fname)
            with open(os.path.join(src, fname), "rb") as f:
                z.writestr(entry("!Hotspot/" + name, filetype, False, now),
                           f.read())

    print("wrote", out)


if __name__ == "__main__":
    main()
