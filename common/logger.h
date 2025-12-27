#ifndef LOGGER_H
#define LOGGER_H

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>

class Logger {
public:
  enum Level { DEBUG, INFO, WARNING, ERROR };

  static void Log(Level level, const std::string& msg) { instance().logInternal(level, msg); }

private:
  static Logger& instance() {
    static Logger inst;
    return inst;
  }

  Logger() = default;
  ~Logger() = default;
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void logInternal(Level level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostream& out = (level == ERROR) ? std::cerr : std::cout;

    out << "[" << std::put_time(std::localtime(&time), "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count() << "] ";

    switch (level) {
      case DEBUG:
        out << "[DEBUG] ";
        break;
      case INFO:
        out << "[INFO]  ";
        break;
      case WARNING:
        out << "[WARN]  ";
        break;
      case ERROR:
        out << "[ERROR] ";
        break;
    }

    out << msg << std::endl;
  }

  std::mutex m_mutex;
};

#endif  // LOGGER_H
