#include <QApplication>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <zmq.hpp>

#include "broker.pb.h"
#include "messagekeys.h"

#include "mainwindow.h"

// ANSI Color Codes for the Terminal UI
#define RESET "\033[0m"
#define DIM "\033[2m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"
#define WHITE "\033[37m"

std::string getCurrentTime() {
  auto now = std::chrono::system_clock::now();
  time_t rawtime = std::chrono::system_clock::to_time_t(now);
  struct tm* timeinfo = localtime(&rawtime);
  char buffer[80];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", timeinfo);

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  return std::string(buffer) + "." + std::to_string(ms.count());
}

int main(int argc, char* argv[]) {
  QApplication a(argc, argv);

  MainWindow mw;
  mw.show();

  return a.exec();
}