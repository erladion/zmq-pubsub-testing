add_executable(server
  main.cpp
  zmqbroker.cpp
  ../zmq.hpp
)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

target_link_libraries(server
    Qt${QT_VERSION_MAJOR}::Core
    zmq
    ZmqLib
)
