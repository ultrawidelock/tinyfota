# SPDX-FileCopyrightText: 2026 Aseem Chopra
# SPDX-License-Identifier: Apache-2.0

# The settings only NCS has. Upstream Zephyr refuses a .conf that assigns a
# symbol it does not define, so these cannot sit in prj.conf or
# sysbuild/mcuboot.conf. Sysbuild includes this file with the workspace's
# modules known, and only NCS has the nrf module.
if(DEFINED ZEPHYR_NRF_MODULE_DIR)
  # PSA crypto from Oberon, NCS's software provider (see prj.conf).
  set_config_bool(${DEFAULT_IMAGE} CONFIG_NRF_SECURITY y)
  # The second banner that selects PRINTK (see sysbuild/mcuboot.conf).
  set_config_bool(mcuboot CONFIG_NCS_BOOT_BANNER n)
  # Lock MCUboot's own region after it hands over. FPROTECT_APP stays OFF: the
  # application writes patch_staging and settings_storage at runtime, and
  # protecting the app region would take them with it. Upstream Zephyr has no
  # such driver, so there MCUboot's region stays writable.
  set_config_bool(mcuboot CONFIG_FPROTECT y)
else()
  # Upstream Zephyr: the header check runs in mbedTLS's p256-m driver, which
  # verifies on the stack. Without it mbedTLS's generic P-256 code fails every
  # verify on prj.conf's 1,024 B heap, and still failed on 4,096 B; with it a
  # verify peaks at 404 B of heap. Measured with ports/zephyr/crypto_psa.c and
  # a real signature, in QEMU on qemu_cortex_m3, Zephyr v4.4.2. The Bluetooth
  # host implies p256-m too; this keeps the verify from depending on that.
  set_config_bool(${DEFAULT_IMAGE} CONFIG_MBEDTLS_PSA_P256M_DRIVER_ENABLED y)
endif()
