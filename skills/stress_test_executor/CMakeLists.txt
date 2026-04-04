cmake_minimum_required(VERSION 3.16)
project(stress_test_executor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(VELIX_SDK_ROOT "${CMAKE_SOURCE_DIR}/../../runtime/sdk/cpp")
set(VELIX_COMM_ROOT "${CMAKE_SOURCE_DIR}/../../communication")

add_executable(main
    main.cpp
    ${VELIX_SDK_ROOT}/velix_process.cpp
    ${VELIX_COMM_ROOT}/send.cpp
    ${VELIX_COMM_ROOT}/recv.cpp
)

target_include_directories(main PRIVATE
    "${CMAKE_SOURCE_DIR}/../../"
    "${VELIX_SDK_ROOT}"
    "${VELIX_COMM_ROOT}"
    "${CMAKE_SOURCE_DIR}/../../vendor"
)

if(WIN32)
    target_link_libraries(main PRIVATE ws2_32)
elseif(APPLE)
    target_link_libraries(main PRIVATE pthread)
else()
    target_link_libraries(main PRIVATE pthread)
endif()

target_compile_options(main PRIVATE
    -Wall -Wextra
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-g -O0>
)

install(TARGETS main DESTINATION .)
install(FILES manifest.json DESTINATION .)
