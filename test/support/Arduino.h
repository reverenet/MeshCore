#pragma once

// Only Crypto's RNG.cpp pulls this in; the AES and SHA256 sources we actually exercise
// are self-contained. It just has to compile - nothing in these tests uses it.
#include "../mocks/Arduino.h"

inline uint32_t micros() { return millis() * 1000; }
