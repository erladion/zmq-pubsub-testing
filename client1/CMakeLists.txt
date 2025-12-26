add_executable(client1
  main.cpp
  ../zmq.hpp
  ../zmq_addon.hpp
)

target_link_libraries(client1
    Qt${QT_VERSION_MAJOR}::Core
    ZmqLib
    zmq
)
