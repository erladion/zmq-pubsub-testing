cmake_minimum_required(VERSION 3.14)

project(zeromq LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

find_package(ZeroMQ QUIET)

if (NOT ZeroMQ_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZeroMQ REQUIRED libzmq)
endif()

include_directories(common)

add_library(ZmqLib STATIC
    common/zmq_client_manager.cpp
    common/zmq_client_manager.h
    common/zmq_common.h
)

target_link_libraries(ZmqLib PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    zmq
)

add_executable(server
  server/main.cpp
  server/zmq_broker.cpp
  common/zmq_common.h
  common/zmq_client_manager.h
  common/zmq_client_manager.cpp
  zmq.hpp
)
target_link_libraries(server Qt${QT_VERSION_MAJOR}::Core zmq)

add_executable(client1
  client1/main.cpp
  common/zmq_common.h
  common/zmq_client_manager.h
  common/zmq_client_manager.cpp
  zmq.hpp
  zmq_addon.hpp
)
target_link_libraries(client1 Qt${QT_VERSION_MAJOR}::Core zmq)

add_executable(client2
  client2/main.cpp
  common/zmq_common.h
  common/zmq_client_manager.h
  common/zmq_client_manager.cpp
  zmq.hpp
  zmq_addon.hpp
)
target_link_libraries(client2 Qt${QT_VERSION_MAJOR}::Core zmq)

include(GNUInstallDirs)
install(TARGETS server client1 client2
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
