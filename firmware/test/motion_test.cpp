// motion_test.cpp — runs the gesture engine against scripted pulls on a laptop.
//
//   g++ -std=c++17 -Wall -Wextra -I../holder motion_test.cpp ../holder/bs_motion.cpp -o motion_test && ./motion_test
//
// Each scenario is a list of (t_ms, delta_mm) samples at a 10 ms poll, followed by
// 2 s of stillness. The expected event string is asserted exactly.
#include "bs_motion.h"
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>

using namespace bs;

static std::string run(const std::vector<std::pair<uint32_t, float>>& samples, uint32_t tail_ms = 2000) {
  MotionEngine m;
  std::string out;
  uint32_t t = 0;
  auto drain = [&]() {
    Event e;
    while (m.next_event(e)) {
      switch (e.type) {
        case EventType::PULL_STARTED:  out += "S"; break;
        case EventType::ITEM:          out += "I" + std::to_string(e.index); break;
        case EventType::PULL_ENDED:    out += "E"; break;
        case EventType::SESSION_ENDED: out += "X"; break;
        default: break;
      }
      out += " ";
    }
  };
  for (auto& s : samples) {
    while (t < s.first) { t += 10; m.tick(t); drain(); }
    m.feed(s.second, t); drain();
  }
  for (uint32_t end = t + tail_ms; t < end;) { t += 10; m.tick(t); drain(); }
  if (!out.empty()) out.pop_back();
  return out;
}

// A steady pull of `mm` over `ms`, starting at t0, as 10 ms samples.
static std::vector<std::pair<uint32_t, float>> pull(uint32_t t0, float mm, uint32_t ms) {
  std::vector<std::pair<uint32_t, float>> v;
  int n = ms / 10; float per = mm / n;
  for (int i = 1; i <= n; ++i) v.push_back({t0 + i * 10u, per});
  return v;
}
static void append(std::vector<std::pair<uint32_t, float>>& a, const std::vector<std::pair<uint32_t, float>>& b) {
  a.insert(a.end(), b.begin(), b.end());
}

struct Case { const char* name; std::vector<std::pair<uint32_t, float>> s; const char* expect; };

int main() {
  std::vector<Case> cases;

  // Three sheets in 1.2 s: start, 3 items, end, session end.
  cases.push_back({"normal3", pull(0, 342, 1200), "S I1 I2 I3 E X"});

  // One sheet.
  cases.push_back({"single", pull(0, 114, 400), "S I1 E X"});

  // A short tug of 50 mm still gets one item.
  cases.push_back({"shorttug", pull(0, 50, 200), "S I1 E X"});

  // A 4 mm bump is below the noise floor: nothing at all.
  cases.push_back({"bump", {{10, 4.0f}}, ""});

  // Hesitant: 100 mm, pause 500 ms (> settle, < idle), then 100 mm more.
  // One session: the second part does not pay the 45 mm first trigger again,
  // and total 200 mm = first(45) + 1 sheet(114) -> 2 items.
  { auto s = pull(0, 100, 400); append(s, pull(900, 100, 400));
    cases.push_back({"hesitant", s, "S I1 E S I2 E X"}); }

  // Two pulls separated by 2 s: two independent sessions.
  { auto s = pull(0, 114, 400); append(s, pull(2400, 114, 400));
    cases.push_back({"twopulls", s, "S I1 E X S I1 E X"}); }

  // Rewind: pull 120 mm, push 30 mm back, pull 40 mm more -> 130 mm, still 1 item
  // (reverse credit clamps travel but never un-fires).
  { auto s = pull(0, 120, 400); append(s, pull(410, -30, 100)); append(s, pull(520, 40, 150));
    cases.push_back({"rewind", s, "S I1 E X"}); }

  // Runaway: a metre of paper still queues only max_queue (4) items.
  cases.push_back({"runaway", pull(0, 1000, 2000), "S I1 I2 I3 I4 E X"});

  // Creep: 1 mm every 100 ms for 2 s. Crosses the noise floor at 6 mm (the amp
  // wakes) but only reaches 20 mm, so no item is ever owed.
  { std::vector<std::pair<uint32_t, float>> s;
    for (int i = 1; i <= 20; ++i) s.push_back({i * 100u, 1.0f});
    cases.push_back({"creep", s, "S E X"}); }

  int fails = 0;
  printf("%-10s %-40s %s\n", "scenario", "events", "result");
  for (auto& c : cases) {
    std::string got = run(c.s);
    bool ok = got == c.expect;
    if (!ok) ++fails;
    printf("%-10s %-40s %s\n", c.name, got.c_str(), ok ? "ok" : (std::string("FAIL (expected '") + c.expect + "')").c_str());
  }
  printf("%zu scenarios, %d failure(s)\n", cases.size(), fails);
  return fails ? 1 : 0;
}
