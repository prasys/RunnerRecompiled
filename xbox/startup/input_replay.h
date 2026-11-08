#pragma once
// Pad input record / replay for repeatable benchmarks (2026-11-02): the user plays a route once
// from a checkpoint (ME_INPUT_RECORD=<file>), later runs replay it (ME_INPUT_REPLAY=<file>), so
// every A/B measures the same place without the user.
// Time base: milliseconds since an anchor - the first moment after a level scene appears (not a
// menu) at which the game has produced 60 swaps within 2.5 s, i.e. the loading screen is over.
// (Guest frames were the first time base: a replay diverged early, because frames in which the
// recording hitched on new pipelines advance the game by more time - UE3 moves by real delta.)
// File: text lines "ms buttons lt rt lx ly rx ry", one per state change.
// During replay the real pad is ignored; after the last line the pad reads idle and
// "input_replay_done" is logged.
#include "guest_input.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" uint64_t me_guest_swaps();
extern "C" void me_current_scene(char* out, size_t capacity);
extern "C" bool me_camera_position(float out[3]);

class MeInputTape {
  struct Entry { uint64_t frame; MeInputSnapshot state; };
  enum Mode { kOff, kRecord, kReplay } mode_ = kOff;
  FILE* record_ = nullptr;
  std::vector<Entry> replay_;
  size_t next_ = 0;
  bool anchored_ = false, done_ = false;
  uint64_t anchor_swaps_ = 0, window_swaps_ = 0;
  std::chrono::steady_clock::time_point window_start_{}, anchor_time_{};
  MeInputSnapshot last_{}, current_{};
  uint32_t sequence_ = 1;
  uint64_t next_position_ms_ = 0;

  static bool in_level() {
    char scene[48] = {};
    me_current_scene(scene, sizeof(scene));
    return scene[0] && !std::strstr(scene, "Menu") && std::strcmp(scene, "startup");
  }
  // Anchor: 60 swaps within 2.5 s while a level scene is current.
  bool anchor() {
    if (anchored_) return true;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t swaps = me_guest_swaps();
    if (!in_level()) { window_swaps_ = swaps; window_start_ = now; return false; }
    if (swaps - window_swaps_ >= 60) {
      if (now - window_start_ <= std::chrono::milliseconds(2500)) {
        anchored_ = true; anchor_swaps_ = swaps; anchor_time_ = now;
        std::printf("{\"event\":\"input_tape_anchor\",\"mode\":\"%s\",\"swap\":%llu}\n",
                    mode_ == kRecord ? "record" : "replay", (unsigned long long)swaps);
        return true;
      }
      window_swaps_ = swaps; window_start_ = now;
    }
    return false;
  }

 public:
  MeInputTape() {
    if (const char* path = std::getenv("ME_INPUT_REPLAY")) {
      if (FILE* f = std::fopen(path, "r")) {
        unsigned long long frame; unsigned buttons, lt, rt; int ax[4];
        while (std::fscanf(f, "%llu %u %u %u %d %d %d %d", &frame, &buttons, &lt, &rt, &ax[0], &ax[1], &ax[2], &ax[3]) == 8) {
          Entry e{frame, {}};
          e.state.connected = true; e.state.buttons = uint16_t(buttons); e.state.lt = uint8_t(lt); e.state.rt = uint8_t(rt);
          for (int i = 0; i < 4; ++i) e.state.axes[i] = int16_t(ax[i]);
          replay_.push_back(e);
        }
        std::fclose(f);
        mode_ = kReplay;
      }
      std::printf("{\"event\":\"input_tape\",\"mode\":\"replay\",\"entries\":%zu}\n", replay_.size());
    } else if (const char* path = std::getenv("ME_INPUT_RECORD")) {
      record_ = std::fopen(path, "w");
      if (record_) mode_ = kRecord;
      std::printf("{\"event\":\"input_tape\",\"mode\":\"record\",\"open\":%s}\n", record_ ? "true" : "false");
    }
  }
  // Called with the live pad state on every guest poll; returns the state the guest gets.
  MeInputSnapshot filter(const MeInputSnapshot& live) {
    if (mode_ == kOff || !anchor()) return live;
    const uint64_t frame = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - anchor_time_).count());  // ms since the anchor
    // Trajectory once a second (record and replay alike) to see where a replay diverges.
    if (frame >= next_position_ms_) {
      next_position_ms_ = frame + 1000;
      float camera[3];
      if (me_camera_position(camera))
        std::printf("{\"event\":\"input_tape_pos\",\"ms\":%llu,\"camera\":[%.0f,%.0f,%.0f]}\n",
                    (unsigned long long)frame, camera[0], camera[1], camera[2]);
    }
    if (mode_ == kRecord) {
      if (live.buttons != last_.buttons || live.lt != last_.lt || live.rt != last_.rt ||
          std::memcmp(live.axes, last_.axes, sizeof(live.axes)) || live.connected != last_.connected) {
        last_ = live;
        std::fprintf(record_, "%llu %u %u %u %d %d %d %d\n", (unsigned long long)frame, unsigned(live.buttons),
                     unsigned(live.lt), unsigned(live.rt), live.axes[0], live.axes[1], live.axes[2], live.axes[3]);
        std::fflush(record_);
      }
      return live;
    }
    while (next_ < replay_.size() && replay_[next_].frame <= frame) {
      current_ = replay_[next_++].state;
      current_.sequence = ++sequence_;
    }
    if (next_ >= replay_.size() && !done_ && (replay_.empty() || frame >= replay_.back().frame + 1000)) {
      done_ = true;
      current_ = MeInputSnapshot{}; current_.connected = true; current_.sequence = ++sequence_;
      std::printf("{\"event\":\"input_replay_done\",\"ms\":%llu}\n", (unsigned long long)frame);
    }
    MeInputSnapshot out = current_;
    out.connected = true;
    return out;
  }
};
inline MeInputTape& me_input_tape() { static MeInputTape tape; return tape; }
