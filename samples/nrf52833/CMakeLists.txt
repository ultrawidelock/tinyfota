cmake_minimum_required(VERSION 3.20.0)

# Build with sysbuild from the root of an upstream Zephyr v4.4.2 or NCS v3.3.0
# workspace; see README.md.
# sysbuild/CMakeLists.txt puts the tinyfota module on both images.
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinyfota_sample)

target_sources(app PRIVATE src/main.c)
