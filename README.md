# potluck-amiga

An Amiga 500 emulator as a [Potluck](https://github.com/Tubifix77/potluck) app: it runs on core 1 of one ESP32-S3
board in a Potluck cell, beside Potluck on core 0, and sends its frames to a PC over the board's Wi-Fi. It is
Potluck's M27 experiment, and the question it answers is the radio's: what does streaming an emulator's frames cost
the cell's links?

- **The emulator** is [amiga500-esp32](https://github.com/Makerstown/amiga500-esp32) (a Musashi 68000 and an OCS
  chipset written from scratch), with its display, sound and arcade-cabinet pins left out. See [NOTICE](NOTICE).
- **No Commodore software.** The board boots this project's own test ROM (`rom/make_test_rom.py`): a 68000 program
  that draws stripes and 52 copper colour bars and moves them every frame. A Kickstart ROM someone may lawfully use
  can take its place, from the PC; none is ever part of this repository or of a firmware image.
- **The ROM comes from the PC**, over Potluck's bulk channel (M25): the cluster file's `bulk.folder` holds it, and the
  board reads it into PSRAM at start.

## Layout

| | |
|---|---|
| `firmware/components/pot_amiga/` | the Potluck app component (descriptor `potluck-modules.json`), the emulator in `third_party/` |
| `pc/` | a PC harness: the same emulator sources run a ROM for N frames and write PPM images |
| `rom/make_test_rom.py` | the test ROM |
| `tools/` | the PC's frame receiver |

## The PC harness

Built with Potluck's host toolchain (MSVC, or gcc/clang):

```
pwsh <potluck>\tools\run_host_tests.ps1 -Source pc -BuildDir pc\build -Release -Target a500_pc
python rom\make_test_rom.py rom\testrom.bin
pc\build\a500_pc.exe rom\testrom.bin 100 --every 50 pc\out\f
```

## Licence

MIT for this project's own code (LICENSE); the third-party code keeps its own (NOTICE).
