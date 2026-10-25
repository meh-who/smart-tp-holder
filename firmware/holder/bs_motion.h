// bs_motion.h — gesture engine for the toilet-roll holder.
//
// Pure C++: no Arduino, ESP-IDF or FreeRTOS dependency, so the same file
// compiles on a laptop (see firmware/test/) and on the ESP32-S3.
//
// Input : surface travel of the paper in millimetres (signed) and a clock.
// Output: a small stream of events the firmware acts on.
//
//   PULL_STARTED   the paper has moved past the noise floor — wake the amp
//   ITEM           one more item is owed (fires once per sheet pulled)
//   PULL_ENDED     the paper has been still for `settle_ms`
//   SESSION_ENDED  still for `idle_ms` — travel resets; next pull starts fresh
//
// Semantics that matter (these are the numbers the whole feel depends on):
//   noise_floor_mm   6     wobble below this is not a pull
//   first_trigger_mm 45    the first item fires well before a full sheet,
//                          so a short tug still gets an answer quickly
//   sheet_mm         114   every further item is one sheet of travel
//   settle_ms        350   gap that ends a pull (hesitation shorter than this
//                          is still the same pull)
//   idle_ms          1500  gap that ends the session and resets travel
//   max_queue        4     items per session, however far you pull —
//                          the backlog cap is deliberately loose (see paper §6)
//   reverse credit         paper pushed back reduces travel (clamped at 0) but
//                          never un-fires an item.
#pragma once
#include <cstdint>

namespace bs {

struct MotionConfig {
  float    noise_floor_mm   = 6.0f;
  float    first_trigger_mm = 45.0f;
  float    sheet_mm         = 114.0f;
  uint32_t settle_ms        = 350;
  uint32_t idle_ms          = 1500;
  uint8_t  max_queue        = 4;
};

enum class EventType : uint8_t { NONE = 0, PULL_STARTED, ITEM, PULL_ENDED, SESSION_ENDED };

struct Event {
  EventType type = EventType::NONE;
  uint32_t  at_ms = 0;
  float     travel_mm = 0;   // session travel at the moment of the event
  uint8_t   index = 0;       // ITEM: 1-based index within the session
};

class MotionEngine {
 public:
  explicit MotionEngine(const MotionConfig& c = MotionConfig()) : cfg_(c) {}

  // Feed a movement of `delta_mm` (signed) observed at `now_ms`. Returns the
  // number of events produced; read them with next_event().
  int feed(float delta_mm, uint32_t now_ms);

  // Call periodically even with no motion so settle/idle timers can fire.
  int tick(uint32_t now_ms) { return feed(0.0f, now_ms); }

  bool  next_event(Event& out);
  bool  pulling() const { return state_ == State::PULLING; }
  bool  in_session() const { return state_ != State::IDLE; }
  float travel_mm() const { return travel_; }
  uint8_t items_this_session() const { return items_; }
  uint32_t sheets_total() const { return sheets_total_; }
  const MotionConfig& config() const { return cfg_; }
  void set_config(const MotionConfig& c) { cfg_ = c; }

 private:
  enum class State : uint8_t { IDLE, PULLING, SETTLED };
  void push(EventType t, uint32_t now, uint8_t idx = 0);
  uint8_t items_owed_for(float travel) const;

  MotionConfig cfg_;
  State    state_ = State::IDLE;
  float    travel_ = 0;            // session travel, clamped >= 0
  float    unconfirmed_ = 0;       // movement before the noise floor is crossed
  uint8_t  items_ = 0;             // items fired this session
  uint32_t last_motion_ms_ = 0;
  uint32_t sheets_total_ = 0;      // lifetime counter (whole sheets)
  float    sheets_acc_ = 0;        // partial sheet accumulator for the lifetime counter

  static constexpr int kQ = 8;
  Event q_[kQ];
  uint8_t q_head_ = 0, q_tail_ = 0;
};

}  // namespace bs
