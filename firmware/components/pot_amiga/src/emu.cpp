// pot_amiga's emulator task: the Amiga 500 on core 1, its frames' changed lines sent to the PC as UDP.
//
// The frame loop is amiga500-esp32's (its a500_esp32.ino loop()), line by line: copper, Denise, 455 CPU cycles,
// Paula, the CIAs; without its display, sound and cabinet. Paced to the PAL rate, 50 frames a second, or slower when
// a frame takes longer; after every frame the task sleeps at least one tick, so core 1's idle task runs (the task
// watchdog watches it) and Potluck's tasks on core 1 -- statistics, console, background worker -- are never kept out.
//
// The stream (tools/frame_viewer.py): the visible 320 x 256 lores picture, one byte a pixel (RGB 3-3-2), only the
// lines that changed since they were last sent, four to a packet:
//   header  'P' 'A' version(1) n(u8) frame(u32 LE)        -- n = 0: the frame is complete
//   n x     line(u16 LE, 0..255) pixels(320 bytes)
// Every streamed frame also resends 8 lines in rotation, so a viewer started late fills in within 32 frames.

#include <atomic>
#include <cstdio>
#include <cstring>

#include "amiga/amiga.hpp"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

extern "C" {
#include "m68k.h"
}
#include "a500.h"
#include "m68kops.h"

// What amiga500-esp32's sketch defined for its chipset files.
int cur_frame = 0;
extern uint8_t* slow_ram;
extern bool g_row_onscreen[];
void cia_tod_hsync(void);
void cia_tod_vsync(void);
extern "C" void* pot_amiga_table_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

namespace pot {
namespace amiga {

namespace {

constexpr int kLines = 312, kCycLine = 455;
constexpr int kTop = 44, kHeight = 256, kWidth = 320;  // DIWSTRT's usual 0x2C: lines 44..299
constexpr int kPerPacket = 4;
constexpr size_t kHeader = 8, kEntry = 2 + kWidth;
constexpr int64_t kFrameUs = 20000;                   // PAL: 50 frames a second
constexpr int kRefreshRows = 8;

uint8_t* g_shadow = nullptr;                          // the lines as last sent (PSRAM)
uint8_t g_pkt[kHeader + kPerPacket * kEntry];
int g_pkt_n = 0;
uint32_t g_frame = 0;
bool g_streaming = false;

int g_sock = -1;
sockaddr_in g_to{};
int64_t g_net_checked = 0;

TaskHandle_t g_task = nullptr;
std::atomic<bool> g_run{false}, g_alive{false};
std::atomic<uint8_t> g_every{0};
std::atomic<uint32_t> g_frames{0}, g_sent{0}, g_bytes{0}, g_packets{0}, g_dropped{0}, g_no_net{0}, g_frame_us{0};
Config g_cfg{};

bool net_ready() {
    if (g_sock >= 0) return true;
    const int64_t now = esp_timer_get_time();
    if (now - g_net_checked < 1000000) return false;
    g_net_checked = now;
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (sta == nullptr || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);  // a full buffer drops the packet, never stalls the emulator
    g_to.sin_family = AF_INET;
    g_to.sin_port = htons(g_cfg.port);
    std::memcpy(&g_to.sin_addr.s_addr, g_cfg.ip, 4);
    g_sock = s;
    return true;
}

void send_packet() {
    g_pkt[0] = 'P';
    g_pkt[1] = 'A';
    g_pkt[2] = 1;
    g_pkt[3] = static_cast<uint8_t>(g_pkt_n);
    std::memcpy(g_pkt + 4, &g_frame, 4);
    const size_t len = kHeader + static_cast<size_t>(g_pkt_n) * kEntry;
    if (sendto(g_sock, g_pkt, len, 0, reinterpret_cast<const sockaddr*>(&g_to), sizeof(g_to)) == static_cast<int>(len)) {
        g_bytes.fetch_add(static_cast<uint32_t>(len));
        g_packets.fetch_add(1);
    } else {
        g_dropped.fetch_add(1);
    }
    g_pkt_n = 0;
}

// Denise hands over each line: 12-bit Amiga colours, 320 pixels (lores) or 640 (hires and empty lines).
void line_cb(int v, const uint16_t* px, int w) {
    if (v < 0 || v >= kLines) return;
    if (!g_streaming) {
        g_row_onscreen[v] = false;  // not sent: Denise draws it again when the next streamed frame comes
        return;
    }
    g_row_onscreen[v] = true;
    const int y = v - kTop;
    if (y < 0 || y >= kHeight) return;
    uint8_t* entry = g_pkt + kHeader + static_cast<size_t>(g_pkt_n) * kEntry;
    uint8_t* row = entry + 2;
    const int step = w >= 640 ? 2 : 1;
    for (int x = 0; x < kWidth; ++x) {
        const uint16_t c = px[x * step < w ? x * step : w - 1];
        row[x] = static_cast<uint8_t>((((c >> 9) & 7) << 5) | (((c >> 5) & 7) << 2) | ((c >> 2) & 3));
    }
    uint8_t* sh = g_shadow + static_cast<size_t>(y) * kWidth;
    if (std::memcmp(sh, row, kWidth) == 0) return;
    std::memcpy(sh, row, kWidth);
    entry[0] = static_cast<uint8_t>(y);
    entry[1] = static_cast<uint8_t>(y >> 8);
    if (++g_pkt_n == kPerPacket) send_packet();
}

void run_frame() {
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
        paula_step(paula_acc >> 1);  // Paula counts colour clocks, half the CPU's
        paula_acc &= 1;
        cia_tick(kCycLine / 10);
        cia_tod_hsync();
    }
    cia_tod_vsync();
    intreq_set(5);
}

void task(void*) {
    mem_reset();
    cia_reset();
    custom_reset();
    paula_reset();
    m68k_init();
    if (m68ki_instruction_jump_table == nullptr) {  // its 768 KB of tables did not fit in PSRAM
        g_alive = false;
        g_task = nullptr;
        vTaskDelete(nullptr);
    }
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    denise_line_cb = line_cb;
    std::memset(g_shadow, 0xFF, static_cast<size_t>(kHeight) * kWidth);  // no line was sent yet
    int64_t next = esp_timer_get_time();
    while (g_run) {
        const int64_t t0 = esp_timer_get_time();
        const uint8_t every = g_every;
        g_streaming = false;
        if (every != 0 && g_frame % every == 0) {
            if (net_ready()) {
                g_streaming = true;
                const int r = static_cast<int>((g_frame / every) % (kHeight / kRefreshRows)) * kRefreshRows;
                std::memset(g_shadow + static_cast<size_t>(r) * kWidth, 0xFF, static_cast<size_t>(kRefreshRows) * kWidth);
            } else {
                g_no_net.fetch_add(1);
            }
        }
        run_frame();
        if (g_streaming) {
            if (g_pkt_n > 0) send_packet();
            send_packet();  // n = 0: the frame is complete
            g_sent.fetch_add(1);
        }
        ++g_frame;
        g_frames.fetch_add(1);
        const int64_t t1 = esp_timer_get_time();
        g_frame_us = static_cast<uint32_t>(t1 - t0);
        next += kFrameUs;
        if (next > t1) {
            const TickType_t ticks = pdMS_TO_TICKS((next - t1) / 1000);
            vTaskDelay(ticks > 0 ? ticks : 1);
        } else {
            next = t1;  // behind the PAL rate: run as fast as the core allows, one tick's rest a frame
            vTaskDelay(1);
        }
    }
    denise_line_cb = nullptr;
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    g_alive = false;
    g_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool emu_alloc(uint8_t** rom) {
    if (chip_ram == nullptr) chip_ram = static_cast<uint8_t*>(heap_caps_malloc(CHIP_SIZE, MALLOC_CAP_SPIRAM));
    if (slow_ram == nullptr) slow_ram = static_cast<uint8_t*>(heap_caps_malloc(SLOW_SIZE, MALLOC_CAP_SPIRAM));
    if (kick_rom == nullptr) kick_rom = static_cast<uint8_t*>(heap_caps_malloc(ROM_SIZE, MALLOC_CAP_SPIRAM));
    if (g_shadow == nullptr) {
        g_shadow = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(kHeight) * kWidth, MALLOC_CAP_SPIRAM));
    }
    *rom = kick_rom;
    return chip_ram != nullptr && slow_ram != nullptr && kick_rom != nullptr && g_shadow != nullptr;
}

bool emu_start(const Config& cfg) {
    if (g_alive) return true;
    g_cfg = cfg;
    g_run = true;
    g_alive = true;
    // Core 1 at priority 1: below everything Potluck runs there. Its stack in internal RAM (fast; 8 KB of heap).
    if (xTaskCreatePinnedToCore(&task, "amiga", 8192, nullptr, 1, &g_task, portNUM_PROCESSORS > 1 ? 1 : 0) != pdPASS) {
        g_run = false;
        g_alive = false;
        return false;
    }
    return true;
}

void emu_stop() {
    g_run = false;
    for (int i = 0; i < 40 && g_alive; ++i) vTaskDelay(pdMS_TO_TICKS(5));  // a frame is at most a few tens of ms
}

bool emu_running() { return g_alive; }

void emu_set_stream_every(uint8_t n) { g_every = n; }

void emu_stats(EmuStats* out) {
    out->frames = g_frames;
    out->sent_frames = g_sent;
    out->bytes = g_bytes;
    out->packets = g_packets;
    out->dropped = g_dropped;
    out->no_net = g_no_net;
    out->frame_us = g_frame_us;
}

}  // namespace amiga
}  // namespace pot
