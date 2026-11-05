#pragma once
#include "Arduino.h"
#include <map>
#include <memory>
#include <vector>
// A flat in-memory filesystem with a capacity, rounded to 4 KB blocks like LittleFS.
struct FakeFS { std::map<std::string, std::string> files; size_t capacity = 1950 * 1024; size_t used() const; };
extern FakeFS g_fs;
class File : public Stream {
 public:
  File() {}
  File(std::string path, bool write, bool dir) : path_(path), write_(write), dir_(dir), ok_(true) {
    if (dir_) { for (auto& kv : g_fs.files) if (kv.first.rfind(path_ + "/", 0) == 0) list_.push_back(kv.first); }
    else if (write_) g_fs.files[path_] = "";
    else data_ = g_fs.files[path_];
  }
  explicit operator bool() const { return ok_; }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) override {
    if (!ok_ || !write_) return 0;
    std::string& d = g_fs.files[path_];
    size_t blocks_before = (d.size() + 4095) / 4096, blocks_after = (d.size() + n + 4095) / 4096;
    if (g_fs.used() + (blocks_after - blocks_before) * 4096 > g_fs.capacity) return 0;
    d.append((const char*)b, n); return n;
  }
  int available() override { return (int)(data_.size() - pos_); }
  int read() override { return pos_ < data_.size() ? (uint8_t)data_[pos_++] : -1; }
  void close() { ok_ = false; }
  const char* name() { name_ = path_.substr(path_.rfind('/') + 1); return name_.c_str(); }
  File openNextFile() { if (next_ >= list_.size()) return File(); return File(list_[next_++], false, false); }
 private:
  std::string path_, data_, name_; bool write_ = false, dir_ = false, ok_ = false; size_t pos_ = 0, next_ = 0; std::vector<std::string> list_;
};
struct LittleFSClass {
  bool begin(bool) { return true; }
  bool exists(const char* p) { return g_fs.files.count(p) > 0; }
  bool exists(const String& p) { return exists(p.c_str()); }
  File open(const char* p, const char* mode = "r") {
    std::string s(p);
    if (s == "/m") return File(s, false, true);
    if (mode[0] == 'r' && !g_fs.files.count(s)) return File();
    return File(s, mode[0] == 'w', false);
  }
  File open(const String& p, const char* mode = "r") { return open(p.c_str(), mode); }
  bool remove(const char* p) { return g_fs.files.erase(p) > 0; }
  bool remove(const String& p) { return remove(p.c_str()); }
  bool rename(const String& a, const char* b) { auto it = g_fs.files.find(a.str()); if (it == g_fs.files.end()) return false; g_fs.files[b] = it->second; g_fs.files.erase(a.str()); return true; }
  bool mkdir(const char*) { return true; }
  size_t totalBytes() { return g_fs.capacity; }
  size_t usedBytes() { return g_fs.used(); }
};
extern LittleFSClass LittleFS;
