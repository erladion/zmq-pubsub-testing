#ifndef UUIDHELPER_H
#define UUIDHELPER_H

#include <chrono>
#include <functional>
#include <random>
#include <string>
#include <thread>

inline std::string generateUUID() {
  static const char hex_chars[] = "0123456789abcdef";
  // Seeding mt19937 from a single 32-bit draw would allow only 2^32 possible
  // UUID streams: two processes drawing the same seed emit identical UUID
  // sequences, and the broker dedup then silently drops one side's messages
  // as "already seen". Seed from several entropy words plus per-process and
  // per-thread salt so streams can't coincide.
  thread_local std::mt19937 gen = [] {
    std::random_device rd;
    const auto now = static_cast<unsigned int>(std::chrono::system_clock::now().time_since_epoch().count());
    const auto tid = static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd(), now, tid};
    return std::mt19937(seq);
  }();
  thread_local std::uniform_int_distribution<> dis(0, 15);
  thread_local std::uniform_int_distribution<> dis2(8, 11);

  std::string uuid(36, ' ');
  uuid[8] = '-';
  uuid[13] = '-';
  uuid[18] = '-';
  uuid[23] = '-';
  auto set_hex = [&](int index) { uuid[index] = hex_chars[dis(gen)]; };
  for (int i = 0; i < 8; ++i) {
    set_hex(i);
  }
  for (int i = 9; i < 13; ++i) {
    set_hex(i);
  }
  uuid[14] = '4';
  for (int i = 15; i < 18; ++i) {
    set_hex(i);
  }
  uuid[19] = hex_chars[dis2(gen)];
  for (int i = 20; i < 23; ++i) {
    set_hex(i);
  }
  for (int i = 24; i < 36; ++i) {
    set_hex(i);
  }
  return uuid;
}

#endif  // UUIDHELPER_H
