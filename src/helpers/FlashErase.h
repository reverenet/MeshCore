#pragma once

/**
 * \brief  Wipe the filesystem at boot, for a device too broken to reach the 'erase' CLI.
 *
 * Compiled in only when FLASH_ERASE_BUILD is defined, which is what `make erase-firmware`
 * does. The result is an ordinary firmware image for the same board, so it is delivered
 * exactly like any other - over the UF2 bootloader drive, or over USB - and it needs no
 * debug probe, unlike PlatformIO's own erase target (nrfjprog --eraseall).
 *
 * The hook runs as the FIRST thing in setup(), before the radio, the display, the
 * filesystem loads and anything else that reads stored state. That ordering is the whole
 * point: the devices that need this are exactly the ones where loading a corrupt pref or
 * contact file is what crashes the boot.
 *
 * WHAT IT DOES NOT DO: this formats the filesystem. It does not touch the bootloader or
 * the SoftDevice, so it cannot revive a device whose bootloader is damaged - though
 * neither can a UF2 image, which needs that same bootloader to be copied in at all.
 */

#include <Arduino.h>

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  using namespace Adafruit_LittleFS_Namespace;
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
  #include <nvs_flash.h>
#endif

/**
 * \brief  Format the board's main filesystem.
 *
 * Mirrors what the 'erase' CLI command does through formatFileSystem(), but reaches the
 * filesystem directly so it does not depend on a mesh or datastore having come up.
 */
inline bool flash_erase_primary() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  return LittleFS.format();
#elif defined(ESP32)
  SPIFFS.begin(true);
  bool ok = SPIFFS.format();
  // on esp32 some settings live in NVS, which formatting SPIFFS leaves entirely alone
  return (nvs_flash_erase() == ESP_OK) && ok;
#else
  #error "need to implement flash erase for this platform"
#endif
}

/**
 * \brief  Report the outcome and stop. Never returns.
 */
inline void flash_erase_halt(bool ok) {
  Serial.begin(115200);

  // Repeat forever rather than print once. USB CDC only enumerates a few seconds after
  // boot, so a single line at startup is gone before a console can be opened - the same
  // reason the downloadable erase image sits waiting on the port.
  //
  // Halting is also what keeps an erase build from being a trap: it never starts the
  // mesh, and it never erases a second time, so a board left on this image is merely
  // idle rather than wiping itself on every power cycle.
  for (;;) {
    Serial.println(ok ? "MeshCore flash erase: OK - flash normal firmware now"
                      : "MeshCore flash erase: FAILED");
    delay(2000);
  }
}
