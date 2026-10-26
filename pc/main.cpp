// potluck-amiga's PC harness: the emulator's chipset and 68000 run frames on the PC, so a ROM is checked here
// before a board ever sees it (the original project's rule: the PC build is the ground truth).
//
//   a500_pc <rom> <frames> [--ppm out.ppm] [--every N out_prefix]
//
// The ROM may be shorter than Kickstart's 256 KB: it is padded with zeros, as the board pads what the PC sends it.
// The frame loop is the board's (src/emu.cpp), line by line: copper, Denise, 455 CPU cycles, Paula, the CIAs.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "m68k.h"
}
#include "a500.h"

uint8_t* slow_ram = nullptr;  // the PC build of memory.cpp has chip RAM and the ROM as arrays, not this
int cur_frame = 0;
void cia_tod_hsync(void);
void cia_tod_vsync(void);

static int load_rom(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return -1;
    std::memset(kick_rom, 0, ROM_SIZE);
    const size_t n = std::fread(kick_rom, 1, ROM_SIZE, f);
    std::fclose(f);
    return n > 8 ? static_cast<int>(n) : -1;
}

static void run_frame() {
    constexpr int kLines = 312, kCycLine = 455;
    ++cur_frame;
    input_frame(cur_frame);
    copper_vblank();
    sprite_vblank();
    static int paula_acc = 0;
    for (int line = 0; line < kLines; ++line) {
        vpos = line;
        if (blit_irq_pending) {
            blit_irq_pending = 0;
            intreq_set(6);
        }
        if (disk_irq_pending) {
            disk_irq_pending = 0;
            intreq_set(1);
        }
        copper_run_line();
        denise_render_line();
        m68k_execute(kCycLine);
        paula_acc += kCycLine;
        paula_step(paula_acc >> 1);
        paula_acc &= 1;
        cia_tick(kCycLine / 10);
        cia_tod_hsync();
    }
    cia_tod_vsync();
    intreq_set(5);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "a500_pc <rom> <frames> [--ppm out.ppm] [--every N out_prefix]\n");
        return 2;
    }
    const char* ppm = nullptr;
    const char* prefix = nullptr;
    int every = 0;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ppm") && i + 1 < argc) {
            ppm = argv[++i];
        } else if (!std::strcmp(argv[i], "--every") && i + 2 < argc) {
            every = std::atoi(argv[++i]);
            prefix = argv[++i];
        }
    }
    static uint8_t slow[SLOW_SIZE];
    slow_ram = slow;
    const int n = load_rom(argv[1]);
    if (n < 0) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    mem_reset();
    cia_reset();
    custom_reset();
    paula_reset();
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    std::printf("[RESET] rom %d bytes, PC=%06X SP=%08X\n", n, m68k_get_reg(nullptr, M68K_REG_PC),
                m68k_get_reg(nullptr, M68K_REG_SP));
    const int frames = std::atoi(argv[2]);
    for (int f = 1; f <= frames; ++f) {
        run_frame();
        if (every > 0 && f % every == 0) {
            const std::string p = std::string(prefix) + std::to_string(f) + ".ppm";
            video_write_ppm(p.c_str());
        }
    }
    std::printf("[DONE] %d frames, PC=%06X D7=%u DMACON=%04X\n", frames, m68k_get_reg(nullptr, M68K_REG_PC),
                m68k_get_reg(nullptr, M68K_REG_D7), dmacon);
    if (ppm != nullptr && video_write_ppm(ppm) != 0) return 1;
    return 0;
}
