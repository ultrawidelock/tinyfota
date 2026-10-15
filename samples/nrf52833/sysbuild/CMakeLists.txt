# Sysbuild entry point for the tinyfota sample: Zephyr's template
# (zephyr/share/sysbuild/template/CMakeLists.txt) plus the one setting that must
# be made at the sysbuild level.

cmake_minimum_required(VERSION 3.20.0)

# THE MODULE HAS TO REACH BOTH IMAGES, and they need opposite halves of it: the
# applier is compiled into MCUboot, because an application cannot rewrite the
# flash it is executing from, and the receiver into the application, because
# the bootloader has no radio. An application's own ZEPHYR_EXTRA_MODULES is read
# by the application image only, so the module is added here, before
# find_package(Sysbuild), which is where sysbuild configures the images and
# copies this cache into each of them. When tinyfota is already a project in
# your west manifest, configure with -DTINYFOTA_IN_MANIFEST=y so it is not
# added twice.
get_filename_component(TINYFOTA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TINYFOTA_IN_MANIFEST)
  list(APPEND EXTRA_ZEPHYR_MODULES "${TINYFOTA_ROOT}")
  list(REMOVE_DUPLICATES EXTRA_ZEPHYR_MODULES)
  set(EXTRA_ZEPHYR_MODULES "${EXTRA_ZEPHYR_MODULES}" CACHE STRING "Extra Zephyr modules" FORCE)
endif()

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sysbuild LANGUAGES)
