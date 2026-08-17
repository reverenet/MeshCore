#pragma once

// The advert test env deliberately keeps test/mocks OFF the include path, because the
// AES128 and SHA256 stubs there are no-ops - testing a cipher against them proves
// nothing. This env pulls the real rweather/Crypto instead, and reuses only the
// Arduino Stream stub that Utils.h needs to compile.
#include "../mocks/Stream.h"
