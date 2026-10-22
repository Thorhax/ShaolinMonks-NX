#pragma once
// The ReXGlue SDK's log macros, which the Thorhax intro (port/thorhax_intro) is written with, on the port's log
#include <format>

#include "core/log.h"

#define REXLOG_INFO(...) LOGR("%s", std::format(__VA_ARGS__).c_str())
#define REXLOG_WARN(...) LOGW("%s", std::format(__VA_ARGS__).c_str())
