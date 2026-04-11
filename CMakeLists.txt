cmake_minimum_required(VERSION 3.10)
project(Velix LANGUAGES CXX)

# Use C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization level matching their .sh scripts
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# Find Threads
find_package(Threads REQUIRED)

# Common include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

# Output binaries directly into the 'build/' directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build)

# -----------------------------------------------------------------------------
# Common Communication & SDK Sources
# -----------------------------------------------------------------------------
set(COMM_SOURCES
    communication/send.cpp
    communication/recv.cpp
)

# -----------------------------------------------------------------------------
# Integration Kernel Sources (Core Services)
# -----------------------------------------------------------------------------
set(KERNEL_SOURCES
    core/supervisor.cpp
    core/process_registry.cpp
    core/termination_engine.cpp
    core/bus.cpp
    execution/executioner.cpp
    execution/prepare_runner.cpp
    execution/run_launcher.cpp
    execution/runtime_adapters/runtime_adapters.cpp
    llm/scheduler.cpp
    llm/session_io.cpp
    llm/compacter.cpp
    llm/session_manager.cpp
    core/persistant_applications/application_manager.cpp
    core/persistant_applications/terminal_registry.cpp
    core/persistant_applications/terminal_driver.cpp
    ${COMM_SOURCES}
)

# Target: integration_kernel
add_executable(integration_kernel integration_kernel.cpp ${KERNEL_SOURCES})
target_link_libraries(integration_kernel PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(integration_kernel PRIVATE ws2_32)
endif()

# -----------------------------------------------------------------------------
# Chat Handler Target
# -----------------------------------------------------------------------------
add_executable(chat_handler 
    chat/handler.cpp 
    runtime/sdk/cpp/velix_process.cpp
    llm/session_io.cpp
    llm/compacter.cpp
    llm/session_manager.cpp
    ${COMM_SOURCES}
)
target_link_libraries(chat_handler PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(chat_handler PRIVATE ws2_32)
endif()

# -----------------------------------------------------------------------------
# Terminal Tester (Interactive Diagnostic Tool)
# -----------------------------------------------------------------------------
add_executable(terminal_tester
    tests/terminal_tester.cpp
    core/persistant_applications/terminal_registry.cpp
    core/persistant_applications/terminal_driver.cpp
    communication/send.cpp
    communication/recv.cpp
)
target_link_libraries(terminal_tester PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(terminal_tester PRIVATE ws2_32)
endif()

# -----------------------------------------------------------------------------
# Final summary notes for user
# -----------------------------------------------------------------------------
# This CMakeLists works on Windows (MSVC/MinGW), Linux (GCC/Clang), and macOS.
# To build:
#   mkdir -p build && cd build && cmake .. && make
# OR (multi-platform way):
#   cmake -B build -S .
#   cmake --build build
