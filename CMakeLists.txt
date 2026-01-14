cmake_minimum_required(VERSION 3.10)
project(MyTCPProject)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(include)

# Standard Socket Libraries (Windows vs Unix)
if(WIN32)
    set(LIBS ws2_32)
else()
    set(LIBS "") # Standard socket libs are built-in on Mac/Linux
endif()

# Source files
file(GLOB SOURCES "src/*.cpp")

# Create Executable (We can change this later to be a library + example app)
add_executable(tcp_app ${SOURCES})

target_link_libraries(tcp_app ${LIBS})
