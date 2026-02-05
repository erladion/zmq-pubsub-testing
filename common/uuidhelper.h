#ifndef UUIDHELPER_H
#define UUIDHELPER_H

#include <random>
#include <string>

inline std::string generateUUID() {
  static const char hex_chars[] = "0123456789abcdef";
  thread_local std::random_device rd;
  thread_local std::mt19937 gen(rd());
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
