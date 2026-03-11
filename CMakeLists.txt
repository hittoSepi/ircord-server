cmake_minimum_required(VERSION 3.20)
project(ircord-server VERSION 0.1.0 LANGUAGES CXX)

# C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Export compile commands for tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find vcpkg integration
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# Find dependencies
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(spdlog REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(SQLiteCpp REQUIRED)

# toml11 is header-only, find it differently
find_path(TOML11_INCLUDE_DIR "toml.hpp"
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/external/toml11"
          "${CMAKE_CURRENT_SOURCE_DIR}/vendor/toml11"
          "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
)

if(NOT TOML11_INCLUDE_DIR)
    message(FATAL_ERROR "toml11 not found. Install via vcpkg: vcpkg install toml11")
endif()

# Protobuf compilation
set(PROTO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/proto/ircord.proto")
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# Source files
set(SERVER_SRC
    src/main.cpp
    src/server.cpp
    src/config.cpp
    src/net/listener.cpp
    src/net/session.cpp
    src/net/tls_context.cpp
    src/db/database.cpp
    src/db/user_store.cpp
    src/db/offline_store.cpp
    src/db/file_store.cpp
    src/channel/channel_manager.cpp
    src/commands/command_handler.cpp
    ${PROTO_SRCS}
)

set(SERVER_HDRS
    src/utils/string_utils.hpp
    src/server.hpp
    src/config.hpp
    src/net/listener.hpp
    src/net/session.hpp
    src/net/tls_context.hpp
    src/net/rate_limiter.hpp
    src/db/database.hpp
    src/db/user_store.hpp
    src/db/offline_store.hpp
    src/db/file_store.hpp
    src/channel/channel_manager.hpp
    src/commands/command_handler.hpp
    ${PROTO_HDRS}
)

# Create executable
add_executable(ircord-server ${SERVER_SRC} ${SERVER_HDRS})

# Include directories
target_include_directories(ircord-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}  # For protobuf generated files
    ${TOML11_INCLUDE_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(ircord-server PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    protobuf::libprotobuf
    spdlog::spdlog
    $<IF:$<TARGET_EXISTS:spdlog::spdlog_header_only>,spdlog::spdlog_header_only,>
    unofficial-sodium::sodium
    SQLiteCpp
)

# Platform-specific settings
if(WIN32)
    # Windows-specific: link winsock2
    target_link_libraries(ircord-server PRIVATE ws2_32 crypt32)

    # Disable automatic linking of Boost libraries (we use FindBoost)
    target_compile_definitions(ircord-server PRIVATE BOOST_ALL_NO_LIB)

    # Windows subsystem: console
    set_target_properties(ircord-server PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
else()
    # Unix-like: pthread, rt
    target_link_libraries(ircord-server PRIVATE pthread rt)
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(ircord-server PRIVATE /W4 /WX-)
else()
    target_compile_options(ircord-server PRIVATE -Wall -Wextra -Wpedantic)
endif()


file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/certs)


# Copy config template to build directory
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config/server.toml.example
               ${CMAKE_BINARY_DIR}/server.toml COPYONLY)

# Install rules
install(TARGETS ircord-server
    RUNTIME DESTINATION bin
)

install(FILES config/server.toml.example
    DESTINATION config
    RENAME server.toml
)

# Enable testing
enable_testing()
