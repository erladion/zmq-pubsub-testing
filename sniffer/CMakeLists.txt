add_executable(sniffer
  main.cpp
  ../zmq.hpp
  ../zmq_addon.hpp
)

target_link_libraries(sniffer
    Qt${QT_VERSION_MAJOR}::Core
    ZmqLib
    zmq
)
