// pot_amiga's actor: reads the ROM from the PC, then starts and watches the emulator. See include/amiga/amiga.hpp.

#include "amiga/amiga.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "pot/bulk.hpp"
#include "pot/namespace.hpp"

namespace pot {
namespace amiga {

namespace {

constexpr const char* kLeaves[] = {"state", "fps", "frames", "sent_fps", "kbps", "dropped"};
constexpr ValueType kTypes[] = {ValueType::U32, ValueType::F32, ValueType::U32,
                                ValueType::F32, ValueType::F32, ValueType::U32};
constexpr size_t kOutputs = sizeof(kLeaves) / sizeof(kLeaves[0]);

uint32_t leaf_hash(uint32_t system_prefix, uint16_t node, const char* leaf) {
    char rest[40];
    std::snprintf(rest, sizeof(rest), "node-%04x/amiga/%s", static_cast<unsigned>(node), leaf);
    return path_hash_from(system_prefix, rest);
}

NsDecl output_decl(uint32_t hash, uint16_t node, ValueType type) {
    NsDecl d;
    d.path_hash = hash;
    d.owner_node = node;
    d.type = type;
    d.unit = Unit::None;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;
    d.staleness_bound_ms = 10000;  // published every 2 s
    d.staleness_policy = StalenessPolicy::Informative;
    return d;
}

}  // namespace

bool parse(const ActorDecl& d, Config& out) {
    if (d.type != kEmuType || d.cfg_len != kCfgLen || d.node_id == kPortableNode) return false;
    const uint8_t* p = d.cfg;
    out.system_prefix = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                        (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    out.provider = static_cast<uint16_t>(p[4] | (p[5] << 8));
    std::memcpy(out.ip, p + 6, 4);
    out.port = static_cast<uint16_t>(p[10] | (p[11] << 8));
    out.stream_every = p[12];
    return out.provider != 0 && out.port != 0;
}

uint32_t EmuActor::hash(const char* leaf) const {
    return leaf_hash(cfg_.system_prefix, node_.config().node_id, leaf);
}

EmuActor::~EmuActor() { emu_stop(); }

bool EmuActor::start(uint32_t now_ms) {
    const uint16_t self = node_.config().node_id;
    for (size_t i = 0; i < kOutputs; ++i) {
        if (node_.ns().declare(output_decl(hash(kLeaves[i]), self, kTypes[i])) != NsError::Ok) return false;
    }
    svc_hash_ = path_hash_from(cfg_.system_prefix, "svc/file");
    in_hash_ = hash("in");  // where the PC pushes the ROM's chunks (not a resource: only CALLs come to it)
    if (!emu_alloc(&rom_)) {
        state_ = kFailed;  // no PSRAM: the board runs on without the emulator
    }
    emu_set_stream_every(cfg_.stream_every);
    last_ms_ = now_ms - kRetryMs;  // ask at once
    now_ = prev_at_ = now_ms;
    publish(now_ms);
    return true;
}

void EmuActor::ask(uint32_t now_ms) {
    const PeerLink* p = node_.peers().find_by_node_id(cfg_.provider);
    last_ms_ = now_ms;
    if (p == nullptr || p->state != PeerState::Alive) return;  // the PC is not here: asked again later
    const size_t name_len = std::strlen(kRomName);
    uint8_t args[5 + 16];
    args[0] = kBulkOpen;
    for (int i = 0; i < 4; ++i) args[1 + i] = static_cast<uint8_t>(in_hash_ >> (8 * i));
    std::memcpy(args + 5, kRomName, name_len);
    got_ = size_ = 0;
    xfer_ = 0;
    asking_ = node_.request_call(cfg_.provider, svc_hash_, args, static_cast<uint16_t>(5 + name_len));
    if (asking_ != 0) state_ = kReading;
}

void EmuActor::tick(uint32_t now) {
    now_ = now;
    // M27's lesson: streaming 4.7 Mbit/s, the board could not take a firmware rollout. While an image arrives the
    // stream pauses (the emulator runs on); Potluck firmware before 2026-10-26 has no receiving_image and never pauses.
    const bool updating = board_ != nullptr && board_->receiving_image != nullptr && board_->receiving_image(board_->ctx);
    if (updating != updating_) {
        updating_ = updating;
        emu_set_stream_every(updating ? 0 : cfg_.stream_every);
    }
    if (rom_ != nullptr && !emu_running()) {
        if (state_ == kReading && now - last_ms_ >= kQuietMs) {
            asking_ = 0;
            state_ = kFailed;  // the read stopped: asked again after kRetryMs
        }
        if ((state_ == kAsking || state_ == kFailed) && now - last_ms_ >= kRetryMs) ask(now);
        if (state_ == kReading && size_ != 0 && got_ == size_) {
            std::memset(rom_ + size_, 0, kRomMax - size_);
            state_ = emu_start(cfg_) ? kRunning : kFailed;
            last_ms_ = now;
        }
    }
    if (now - published_at_ >= 2000) publish(now);
}

void EmuActor::publish(uint32_t now) {
    EmuStats s{};
    emu_stats(&s);
    const float dt = static_cast<float>(now - prev_at_) / 1000.0f;
    if (dt >= 1.0f) {
        fps_ = static_cast<float>(s.frames - prev_frames_) / dt;
        sent_fps_ = static_cast<float>(s.sent_frames - prev_sent_) / dt;
        kbps_ = static_cast<float>(s.bytes - prev_bytes_) * 8.0f / 1000.0f / dt;
        prev_frames_ = s.frames;
        prev_sent_ = s.sent_frames;
        prev_bytes_ = s.bytes;
        prev_at_ = now;
    }
    if (state_ == kRunning && !emu_running()) state_ = kFailed;
    node_.ns().publish(hash("state"), Value::of_u32(state_), now);
    node_.ns().publish(hash("fps"), Value::of_f32(fps_), now);
    node_.ns().publish(hash("frames"), Value::of_u32(s.frames), now);
    node_.ns().publish(hash("sent_fps"), Value::of_f32(sent_fps_), now);
    node_.ns().publish(hash("kbps"), Value::of_f32(kbps_), now);
    node_.ns().publish(hash("dropped"), Value::of_u32(s.dropped), now);
    published_at_ = now;
}

bool EmuActor::on_call_result(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, Node::CallOutcome o,
                              const Value& v) {
    if (path_hash != svc_hash_ || from_node != cfg_.provider || msg_id == 0 || msg_id != asking_) return false;
    asking_ = 0;
    uint32_t size = kBulkNoFile;
    if (o != Node::CallOutcome::Ok || !v.as_u32(size) || size == kBulkNoFile || size == 0 || size > kRomMax) {
        state_ = kFailed;  // no amiga.rom in the PC's bulk folder, or not a ROM's size
        return true;
    }
    size_ = size;
    last_ms_ = now_;
    return true;
}

int EmuActor::on_call(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, const uint8_t* args, uint16_t len) {
    if (path_hash != in_hash_) return kCallNotMine;
    BulkChunk c{};
    if (!parse_bulk_chunk(args, len, c)) return kCallRefused;
    if (state_ == kReading && asking_ == 0 && rom_ != nullptr) {
        if (got_ == 0 && c.offset == 0) xfer_ = c.xfer;
        if (c.xfer == xfer_ && c.offset == got_ && got_ + c.len <= size_) {
            std::memcpy(rom_ + got_, c.data, c.len);
            got_ += c.len;
            last_ms_ = now_;
        }
    }
    node_.reply_call(from_node, msg_id, path_hash, Value::of_u32(got_));  // the offset expected next
    return kCallAccepted;
}

bool EmuActor::on_console(const char* line, size_t len) {
    static const char kCmd[] = "POT! amiga stream ";  // the runtime offers the whole line
    const size_t n = sizeof(kCmd) - 1;
    if (len <= n || std::strncmp(line, kCmd, n) != 0) return false;
    const long every = std::strtol(line + n, nullptr, 10);
    if (every < 0 || every > 50) return true;
    cfg_.stream_every = static_cast<uint8_t>(every);
    if (!updating_) emu_set_stream_every(cfg_.stream_every);  // during an update it takes effect afterwards
    std::printf("{\"t\":\"test\",\"cmd\":\"amiga stream\",\"stream_every\":%u}\n",  // console_send.py shows "test" records
                static_cast<unsigned>(cfg_.stream_every));
    return true;
}

size_t EmuActor::stats_json(char* buf, size_t cap, uint32_t) {
    EmuStats s{};
    emu_stats(&s);
    const int n = std::snprintf(
        buf, cap,
        "{\"t\":\"amiga\",\"node\":%u,\"state\":%u,\"rom\":%u,\"every\":%u,\"frames\":%u,\"fps\":%.1f,\"frame_us\":%u,"
        "\"send_us\":%u,\"sent\":%u,\"sent_fps\":%.1f,\"kbps\":%.0f,\"packets\":%u,\"dropped\":%u,\"no_net\":%u}",
        static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(state_), static_cast<unsigned>(size_),
        static_cast<unsigned>(cfg_.stream_every), static_cast<unsigned>(s.frames), static_cast<double>(fps_),
        static_cast<unsigned>(s.frame_us), static_cast<unsigned>(s.send_us), static_cast<unsigned>(s.sent_frames), static_cast<double>(sent_fps_),
        static_cast<double>(kbps_), static_cast<unsigned>(s.packets), static_cast<unsigned>(s.dropped),
        static_cast<unsigned>(s.no_net));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

namespace {

bool check(const ActorDecl& d, const ActorEnv&, const char** why) {
    Config c;
    if (!parse(d, c)) {
        *why = "amiga/emu: pinned; system_prefix u32, provider u16 (not 0), ip0..3 u8, port u16 (not 0), stream_every u8";
        return false;
    }
    return true;
}

Actor* create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(EmuActor) <= kActorSlotBytes, "EmuActor outgrew its actor slot");
    Config c;
    if (env.node == nullptr || !parse(d, c)) return nullptr;
    return new (mem) EmuActor(*env.node, c, env.board);
}

size_t outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    Config c;
    if (peer != 0 || cap < kOutputs || node != d.node_id || !parse(d, c)) return 0;
    for (size_t i = 0; i < kOutputs; ++i) out[i] = output_decl(leaf_hash(c.system_prefix, node, kLeaves[i]), node, kTypes[i]);
    return kOutputs;
}

}  // namespace

ActorKind emu_row() { return {kEmuType, "amiga_emu", &check, &create, nullptr, &outputs}; }

}  // namespace amiga

const ActorKind* app_rows_pot_amiga(size_t* n) {
    static const ActorKind kRows[] = {amiga::emu_row()};
    *n = sizeof(kRows) / sizeof(kRows[0]);
    return kRows;
}

}  // namespace pot
