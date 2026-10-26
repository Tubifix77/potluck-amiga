// pot_amiga: an Amiga 500 emulator as a Potluck app (Potluck's M27 experiment).
//
// ext:amiga/emu (type 0x90, pinned to one board). At start it asks the PC's file service (Potluck's bulk channel,
// M25) for "amiga.rom" and reads it into PSRAM; then it runs the emulator (third_party/: amiga500-esp32's chipset
// and the Musashi 68000) on its own task on core 1, beside Potluck on core 0, and sends each frame's changed lines
// to the PC over the board's Wi-Fi station as UDP (tools/frame_viewer.py receives them). The ROM may be shorter
// than Kickstart's 256 KB (this project's own test ROM is 708 bytes): the rest is zeros.
//
// Outputs, under potluck://<system>/node-<id>/amiga/:
//   state     u32  0 asking for the ROM, 1 reading it, 2 running, 3 failed (no ROM, no PSRAM, or the PC gone)
//   fps       f32  emulated frames a second (the real Amiga's PAL rate is 50)
//   frames    u32  frames emulated since the start
//   sent_fps  f32  frames streamed a second (a frame counts once all its changed lines were handed to the network)
//   kbps      f32  the stream's UDP payload, kilobits a second
//   dropped   u32  packets the network refused (no buffer) since the start
//
// Config (potluck-modules.json order): system_prefix u32, provider u16 (the node offering the file service: the PC,
// 0x00FE), ip0..ip3 u8 (the PC's IPv4 address), port u16 (its UDP port), stream_every u8 (stream every Nth frame;
// 0 streams nothing -- the experiment's baseline). `POT! amiga stream <n>` on the board's console changes it live.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {
namespace amiga {

constexpr ActorType kEmuType = static_cast<ActorType>(0x90);
constexpr uint8_t kCfgLen = 13;
constexpr uint32_t kRomMax = 0x40000;  // Kickstart 1.3's 256 KB, mapped at 0xF80000 and 0xFC0000
constexpr const char* kRomName = "amiga.rom";
constexpr uint32_t kQuietMs = 10000;   // a ROM read with no chunk for this long has failed; asked again later
constexpr uint32_t kRetryMs = 15000;

enum State : uint32_t { kAsking = 0, kReading = 1, kRunning = 2, kFailed = 3 };

struct Config {
    uint32_t system_prefix = 0;
    uint16_t provider = 0;
    uint8_t ip[4] = {};
    uint16_t port = 0;
    uint8_t stream_every = 0;
};

bool parse(const ActorDecl& d, Config& out);

// The emulator's side (src/emu.cpp): one per board, on its own task.
struct EmuStats {
    uint32_t frames;
    uint32_t sent_frames;
    uint32_t bytes;       // UDP payload handed to the network
    uint32_t packets;
    uint32_t dropped;
    uint32_t no_net;      // frames not streamed because the station had no address
    uint32_t frame_us;    // the last frame's time, streaming included
    uint32_t send_us;     // of which in sendto()
};
bool emu_alloc(uint8_t** rom);  // PSRAM for the ROM, the Amiga's RAM and the 68000's tables; false if short
bool emu_start(const Config& cfg);
void emu_stop();
bool emu_running();
void emu_set_stream_every(uint8_t n);
void emu_stats(EmuStats* out);

class EmuActor : public Actor {
  public:
    EmuActor(Node& node, const Config& cfg) : node_(node), cfg_(cfg) {}
    ~EmuActor() override;
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    bool on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                        const Value& v) override;
    int on_call(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, const uint8_t* args, uint16_t len) override;
    bool on_console(const char* line, size_t len) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

  private:
    void ask(uint32_t now_ms);
    void publish(uint32_t now_ms);
    uint32_t hash(const char* leaf) const;

    Node& node_;
    Config cfg_;
    uint8_t* rom_ = nullptr;
    uint32_t state_ = kAsking;
    uint32_t size_ = 0, got_ = 0;
    uint16_t asking_ = 0, xfer_ = 0;
    uint32_t svc_hash_ = 0, in_hash_ = 0;
    uint32_t last_ms_ = 0, now_ = 0, published_at_ = 0;
    uint32_t prev_frames_ = 0, prev_sent_ = 0, prev_bytes_ = 0, prev_at_ = 0;
    float fps_ = 0, sent_fps_ = 0, kbps_ = 0;
};

ActorKind emu_row();

}  // namespace amiga

const ActorKind* app_rows_pot_amiga(size_t* n);

}  // namespace pot
