[![Stand With Ukraine](https://raw.githubusercontent.com/vshymanskyy/StandWithUkraine/main/banner-direct-single.svg)](https://stand-with-ukraine.pp.ua)

# WHDD — HDD diagnostic and recovery tool for Linux

English | [简体中文](README.zh-CN.md)

WHDD is an HDD diagnostic and recovery tool for Linux. Its console UI
(ncurses/libdialog) follows the classic MHDD workflow: whole-drive read
tests with per-block timing visualization, device copying with
fault-tolerant read strategies, SMART reporting and more.

- Project website: http://whdd.github.io
- License: GNU GPL v3
- Sources, bugreports: https://github.com/whdd/whdd

This repository is a modified edition of
[whdd/whdd](https://github.com/whdd/whdd). All upstream features remain
intact; the changes made in this edition are described below.

## Changes in this edition

### Read test
- Fixed 100x40 console layout, anchored to the top-left corner. Resizing
  the terminal during a scan can no longer garble the display: drawing
  simply pauses while the terminal is smaller than the layout and resumes
  automatically once it is large enough again.
- A terminal smaller than the layout is reported with a clear error dialog
  at startup instead of failing silently.
- MHDD-style access-time color scale (dark gray / gray / light gray /
  green / light red / red). The thresholds scale with the read block size,
  so the colors keep their meaning on any block size.
- Selectable read block size: 256, 1024 or 4096 sectors per block
  (128 KiB / 512 KiB / 2 MiB).
- Device model and serial number are shown in the scan screen.

### Scan reports
After a read test, two files are written automatically:
- `WHDD_REPORT_<serial>_<YYMMDD>_<HHMMSS>.report` — summary: parameters,
  speed, access-time histogram, error type counts and the error LBA range.
- `WHDD_REPORT_<serial>_<YYMMDD>_<HHMMSS>.dg` — DiskGenius-style defect
  list grouped by severity: `damaged` (read errors), `severe` (red,
  >= 500 ms) and `slow` (light red, 150..500 ms), each listed as contiguous
  LBA intervals. Thresholds scale with the block size.

### Experimental
A standalone dd-style tool (`whdd-dd`) is being developed on the `dev`
branch: single-LBA block reads with status and timing, through POSIX
`O_DIRECT` reads or ATA READ DMA EXT (SAT), and fully
logical-sector-size aware (512n / 512e / native 4Kn drives).

## Build and install

Debian/Ubuntu dependencies: `./build_depends.sh` (make, gcc, pkgconf,
dialog, libncurses-dev).

```
./build.sh && sudo make install
```

or step by step:

```
make                # release build
make whdd_g         # debug build
sudo make install   # installs to /usr/local/bin
```

CMake is also supported: `cmake . && make`, or `cmake -DSTATIC=ON . && make`
for a static binary (builds ncurses and dialog into `external/`).

Run as root to access block devices.

## License

GNU GPL v3 — see LICENSE. The original WHDD is copyright Andrey Utkin and
contributors; the modifications in this repository are distributed under
the same license.
