cmake_minimum_required(VERSION 3.16)
project(tubelite LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(tubelite src/main.cpp)
target_link_libraries(tubelite PRIVATE SQLite::SQLite3 OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_compile_definitions(tubelite PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
