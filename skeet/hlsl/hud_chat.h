#pragma once

#include "../Json/json.hpp"
#include <cstdint>

// Private Persistence integration, separate from shader execution. No callbacks
// into Lua: accepted chat lines are copied into a bounded native queue.
namespace hud_chat {
nlohmann::json poll(unsigned owner, bool enabled, bool hide, std::uint64_t since);
void release(unsigned owner);
void shutdown();

#ifdef HLSL_STANDALONE_TEST
void test_target(void **table, void *print, void *printEx, void *filter);
void test_time(std::uint64_t milliseconds);
#endif
} // namespace hud_chat
