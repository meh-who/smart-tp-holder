// bs_motion.cpp — see bs_motion.h for the semantics.
#include "bs_motion.h"

namespace bs {

void MotionEngine::push(EventType t, uint32_t now, uint8_t idx) {
  uint8_t next = (q_tail_ + 1) % kQ;
  if (next == q_head_) return;  // full: drop (never happens at sane poll rates)
  q_[q_tail_] = Event{t, now, travel_, idx};
  q_tail_ = next;
}

bool MotionEngine::next_event(Event& out) {
  if (q_head_ == q_tail_) return false;
  out = q_[q_head_];
  q_head_ = (q_head_ + 1) % kQ;
  return true;
}

uint8_t MotionEngine::items_owed_for(float travel) const {
  if (travel < cfg_.first_trigger_mm) return 0;
  float extra = travel - cfg_.first_trigger_mm;
  int n = 1 + static_cast<int>(extra / cfg_.sheet_mm);
  if (n > cfg_.max_queue) n = cfg_.max_queue;
  return static_cast<uint8_t>(n);
}

int MotionEngine::feed(float delta_mm, uint32_t now_ms) {
  int produced = 0;
  const bool moved = (delta_mm != 0.0f);

  // ---- lifetime sheet counter (forward travel only, any state) -------------
  if (delta_mm > 0) {
    sheets_acc_ += delta_mm;
    while (sheets_acc_ >= cfg_.sheet_mm) { sheets_acc_ -= cfg_.sheet_mm; ++sheets_total_; }
  }

  switch (state_) {
    case State::IDLE: {
      if (!moved) break;
      // Accumulate until the noise floor is crossed; wobble decays back to 0.
      unconfirmed_ += delta_mm;
      if (unconfirmed_ < 0) unconfirmed_ = 0;
      if (unconfirmed_ >= cfg_.noise_floor_mm) {
        travel_ = unconfirmed_;
        unconfirmed_ = 0;
        items_ = 0;
        state_ = State::PULLING;
        last_motion_ms_ = now_ms;
        push(EventType::PULL_STARTED, now_ms); ++produced;
        // A big first sample can already owe an item.
        uint8_t owed = items_owed_for(travel_);
        while (items_ < owed) { ++items_; push(EventType::ITEM, now_ms, items_); ++produced; }
      }
      break;
    }

    case State::PULLING: {
      if (moved) {
        travel_ += delta_mm;             // reverse credit: pushing paper back
        if (travel_ < 0) travel_ = 0;    // reduces travel, clamped at zero
        last_motion_ms_ = now_ms;
        uint8_t owed = items_owed_for(travel_);
        while (items_ < owed) { ++items_; push(EventType::ITEM, now_ms, items_); ++produced; }
      } else if (now_ms - last_motion_ms_ >= cfg_.settle_ms) {
        state_ = State::SETTLED;
        push(EventType::PULL_ENDED, now_ms); ++produced;
      }
      break;
    }

    case State::SETTLED: {
      if (moved) {
        // Same session continues: travel and items carry over, so a hesitant
        // pull is not charged the 45 mm first-trigger twice.
        if (delta_mm > 0 || travel_ > 0) {
          travel_ += delta_mm;
          if (travel_ < 0) travel_ = 0;
          state_ = State::PULLING;
          last_motion_ms_ = now_ms;
          push(EventType::PULL_STARTED, now_ms); ++produced;
          uint8_t owed = items_owed_for(travel_);
          while (items_ < owed) { ++items_; push(EventType::ITEM, now_ms, items_); ++produced; }
        }
      } else if (now_ms - last_motion_ms_ >= cfg_.idle_ms) {
        state_ = State::IDLE;
        push(EventType::SESSION_ENDED, now_ms); ++produced;
        travel_ = 0; items_ = 0; unconfirmed_ = 0;
      }
      break;
    }
  }
  return produced;
}

}  // namespace bs
