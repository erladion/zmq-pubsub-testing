set(PROTO_GENERATED_SOURCES
    generated/broker.pb.cc
    generated/update.pb.cc
)

set(PROTO_GENERATED_HEADERS
    generated/broker.pb.h
    generated/update.pb.h
)

find_library(UTF8_RANGE_LIB
    NAMES utf8_range utf8_validity
    PATHS "$ENV{HOME}/.local/lib" "/usr/local/lib" "/usr/lib"
)

if (NOT UTF8_RANGE_LIB)
    message(WARNING "Could not find libutf8_range.a! Linker errors may occur.")
endif()

add_library(ZmqLib STATIC
    ${PROTO_GENERATED_SOURCES}
    ${PROTO_GENERATED_HEADERS}
    zmqworker.h
    zmqworker.cpp
    zmqconnectionmanager.h
    zmqconnectionmanager.cpp
    connectionapi.h
    zmqconnectionapi.cpp
    logger.h
    messagekeys.h
)

target_include_directories(ZmqLib PUBLIC generated)

target_link_libraries(ZmqLib PUBLIC
    zmq
    protobuf::libprotobuf
    absl::base
    absl::strings
    absl::cord
    absl::synchronization
    absl::time
    absl::flags
    absl::log
    absl::check
    absl::status
    absl::statusor
    ${UTF8_RANGE_LIB}
)
