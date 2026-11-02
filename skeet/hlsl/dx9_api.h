#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../Json/json.hpp"
#include <d3d9.h>
#include <vector>
namespace dx9_api {
using json = nlohmann::json;
struct PreparedUpdate {
    unsigned slot = 0, retain = 0;
    std::vector<float> values;
};
struct BufferUpdate {
    unsigned handle = 0;
    size_t offset = 0;
    const char *bytes = nullptr;
    size_t size = 0;
};
json request(unsigned owner, const json &);
bool try_update(unsigned owner, const BufferUpdate &, json &result);
bool try_submit(unsigned owner, PreparedUpdate &, json &result);
// False means transient backend contention; validation failures still throw.
bool try_request(unsigned owner, const json &, json &result);
#ifdef HLSL_STANDALONE_TEST
void test_fail_query(unsigned);
bool test_nested_state(IDirect3DDevice9 *);
void test_lock_backend();
void test_unlock_backend();
#endif
void render(IDirect3DDevice9 *);
void reset();
void shutdown();
} // namespace dx9_api
