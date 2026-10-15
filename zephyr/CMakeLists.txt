# Delta update, bootloader half. Contributes nothing unless
# CONFIG_TINYFOTA_APPLIER=y, so every image that does not ask for it -- which is
# every image except MCUboot -- stays byte-identical.
#
# This module is on the module path of ALL images, not just the application.
# That is how a Zephyr module reaches the bootloader at all: sysbuild builds
# MCUboot as an ordinary Zephyr application, so it loads the same modules, and
# NCS injects its own MCUboot hooks exactly this way
# (nrf/modules/mcuboot/hooks/CMakeLists.txt gates on CONFIG_BOOT_IMAGE_ACCESS_HOOKS).
# The Kconfig gate above is therefore the only thing keeping this out of the
# application image.

get_filename_component(TINYFOTA_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TINYFOTA_CORE ${TINYFOTA_ROOT}/core)
# The Zephyr backends of tinyfota_port.h. The applier needs only the flash one,
# and it is named by path rather than through a library of its own because the
# applier half below is built into MCUboot, where nothing else of the port
# belongs.
set(TINYFOTA_PORT_ZEPHYR ${TINYFOTA_ROOT}/ports/zephyr)

if(CONFIG_TINYFOTA_APPLIER)

  set(DETOOLS ${TINYFOTA_ROOT}/third_party/detools)

  zephyr_library_named(tinyfota)
  zephyr_library_sources(
    ${TINYFOTA_CORE}/dfu_applier.c
    ${TINYFOTA_CORE}/dfu_crc.c
    ${TINYFOTA_PORT_ZEPHYR}/flash_zephyr.c
    ${DETOOLS}/detools.c
    ${DETOOLS}/heatshrink/heatshrink_decoder.c)

  zephyr_library_include_directories(
    ${TINYFOTA_ROOT}/include
    ${TINYFOTA_CORE}
    ${DETOOLS}
    ${DETOOLS}/heatshrink)

  # LZMA is the expensive decompressor and nothing here creates lzma patches;
  # dropping it is most of why this fits in the bootloader at all. `none` is
  # kept because it costs almost nothing and makes an uncompressed patch
  # debuggable on the bench. FILE_IO would pull in stdio.
  zephyr_library_compile_definitions(
    DETOOLS_CONFIG_FILE_IO=0
    DETOOLS_CONFIG_COMPRESSION_LZMA=0
    DETOOLS_CONFIG_COMPRESSION_CRLE=0
    DETOOLS_CONFIG_COMPRESSION_NONE=1
    DETOOLS_CONFIG_COMPRESSION_HEATSHRINK=1)

  # Vendored third-party C, not held to this tree's warning level.
  set_source_files_properties(
    ${DETOOLS}/detools.c
    ${DETOOLS}/heatshrink/heatshrink_decoder.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-sign-compare")

endif()

# The application half. Never both in one image: the applier belongs to the
# bootloader and the receiver to the application, and each is inert without the
# other.
if(CONFIG_TINYFOTA_RECEIVER)

  # The update-signing key is the IMAGE-signing key, so there is one secret to
  # hold and no way for the two to drift. Passed from the sysbuild command line
  # because SB_CONFIG_BOOT_SIGNATURE_KEY_FILE is evaluated a level up and is
  # not visible to an image's CMake.
  # zephyr_get(), not a plain read. A -D on the west command line lands in the
  # SYSBUILD cache, and an image's CMake only sees that through this accessor --
  # which is exactly how EXTRA_ZEPHYR_MODULES reaches the bootloader
  # (zephyr/cmake/modules/zephyr_module.cmake:39). Reading the bare variable
  # finds nothing and reports a missing key that was in fact passed.
  zephyr_get(TINYFOTA_KEY)

  # UNLESS ONE IS PASSED, THE KEY THIS IMAGE IS SIGNED WITH. sysbuild hands
  # SB_CONFIG_BOOT_SIGNATURE_KEY_FILE to the application image as
  # CONFIG_MCUBOOT_SIGNATURE_KEY_FILE (zephyr/share/sysbuild/image_configurations/
  # MAIN_image_default.cmake), and that is visible here even though the
  # SB_CONFIG_ symbol is not. Same key, by construction.
  if(NOT TINYFOTA_KEY AND IS_ABSOLUTE "${CONFIG_MCUBOOT_SIGNATURE_KEY_FILE}")
    set(TINYFOTA_KEY "${CONFIG_MCUBOOT_SIGNATURE_KEY_FILE}")
  endif()

  if(NOT TINYFOTA_KEY)
    message(FATAL_ERROR
      "CONFIG_TINYFOTA_RECEIVER is set but TINYFOTA_KEY is not.\n"
      "  The receiver verifies a staged update against the public half of the\n"
      "  image-signing key, so it cannot be built without knowing which key.\n"
      "  Build with sysbuild and MCUboot, or pass\n"
      "  -DTINYFOTA_KEY=<absolute path to the signing key>.")
  endif()
  # MCUboot's published development keys. Every stock MCUboot accepts images
  # signed with them, so anyone can sign a patch this board will stage.
  get_filename_component(tinyfota_key_name "${TINYFOTA_KEY}" NAME)
  if(tinyfota_key_name MATCHES "^root-(ec-p256|ed25519|rsa-2048|rsa-3072)\\.pem$")
    message(WARNING
      "tinyfota: updates are signed with MCUboot's published development key\n"
      "  (${TINYFOTA_KEY}). Anyone can sign a patch this board accepts. Fine on a\n"
      "  bench; set SB_CONFIG_BOOT_SIGNATURE_KEY_FILE to your own key before shipping.")
  endif()
  if(NOT EXISTS "${TINYFOTA_KEY}")
    message(FATAL_ERROR "TINYFOTA_KEY does not exist: ${TINYFOTA_KEY}")
  endif()

  set(tinyfota_pubkey_c ${CMAKE_CURRENT_BINARY_DIR}/tinyfota_pubkey.c)
  add_custom_command(
    OUTPUT ${tinyfota_pubkey_c}
    COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/emit_pubkey.py
            "${TINYFOTA_KEY}" ${tinyfota_pubkey_c}
    DEPENDS "${TINYFOTA_KEY}" ${CMAKE_CURRENT_SOURCE_DIR}/scripts/emit_pubkey.py
    COMMENT "tinyfota: extracting the update-signing public key")

  zephyr_library_named(tinyfota)
  zephyr_library_sources(
    ${TINYFOTA_CORE}/dfu_receiver.c
    ${TINYFOTA_CORE}/dfu_crc.c
    ${TINYFOTA_PORT_ZEPHYR}/flash_zephyr.c
    ${TINYFOTA_PORT_ZEPHYR}/port_zephyr.c
    ${TINYFOTA_PORT_ZEPHYR}/crypto_psa.c
    ${tinyfota_pubkey_c})
  zephyr_library_sources_ifdef(CONFIG_TINYFOTA_BLE ${TINYFOTA_PORT_ZEPHYR}/dfu_ble_zephyr.c)
  # The optional SMP image-group front door onto this receiver; see the
  # Kconfig help for why Zephyr's own img_mgmt cannot be used here.
  zephyr_library_sources_ifdef(CONFIG_TINYFOTA_SMP_IMG ${TINYFOTA_PORT_ZEPHYR}/dfu_smp_img.c)

  # zephyr_include_directories, not the _library_ form: the application calls
  # tinyfota_window_open() and has to include tinyfota_rx.h. The _library_
  # form scopes the path to this module's own library only, and the app then
  # fails to find the header.
  zephyr_include_directories(${TINYFOTA_ROOT}/include ${TINYFOTA_CORE})

endif()
