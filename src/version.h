// version.h — firmware identity for OTA (see OTA.md §1).
//
// FW_VERSION is the semver the API's /firmware/latest compares against
// (device sends it as ?v=) and the BLE snapshot reports as the trailing
// fw: field. Bump it for every release; the CI release workflow (tag
// fw-v*) reads it to name the binaries.
//
// FW_BUILD is the git short hash, injected by tools/version_flags.py
// (PlatformIO pre-script). "dev" when git is unavailable.

#pragma once

#ifndef FW_BUILD
#define FW_BUILD "dev"
#endif

constexpr const char* FW_VERSION = "1.0.0";
