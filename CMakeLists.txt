cmake_minimum_required(VERSION 3.10)
project(Velix LANGUAGES C CXX)

# Use C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization level matching their .sh scripts
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# Find Threads
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# SQLite dependency (embedded preferred)
# -----------------------------------------------------------------------------
set(VELIX_SQLITE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite/sqlite3.c")
set(VELIX_SQLITE_HDR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite/sqlite3.h")

if(EXISTS "${VELIX_SQLITE_SRC}" AND EXISTS "${VELIX_SQLITE_HDR}")
    message(STATUS "Using embedded SQLite amalgamation from vendor/sqlite")

    add_library(sqlite3 STATIC ${VELIX_SQLITE_SRC})
    target_include_directories(sqlite3 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite
    )
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_THREADSAFE=1
        SQLITE_ENABLE_FTS5
        SQLITE_ENABLE_JSON1
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_OMIT_DEPRECATED
    )

    set(VELIX_SQLITE_TARGET sqlite3)
else()
    message(WARNING "Embedded SQLite amalgamation not found at vendor/sqlite; falling back to system SQLite3")
    find_package(SQLite3 REQUIRED)
    set(VELIX_SQLITE_TARGET SQLite::SQLite3)
endif()

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
    llm/storage/json_storage_provider.cpp
    llm/storage/provider_factory.cpp
    llm/storage/sqlite_storage_provider.cpp
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
target_link_libraries(integration_kernel PRIVATE ${VELIX_SQLITE_TARGET})
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
    llm/storage/json_storage_provider.cpp
    llm/storage/provider_factory.cpp
    llm/storage/sqlite_storage_provider.cpp
    llm/compacter.cpp
    llm/session_manager.cpp
    ${COMM_SOURCES}
)
target_link_libraries(chat_handler PRIVATE Threads::Threads)
target_link_libraries(chat_handler PRIVATE ${VELIX_SQLITE_TARGET})
if(WIN32)
    target_link_libraries(chat_handler PRIVATE ws2_32)
endif()
