#include "compiler6.h"
#include <Windows.h>
#include <cstring>
#include <d3dcommon.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
namespace shader6 {
namespace fs = std::filesystem;
static std::string utf8(const fs::path &p) {
    auto s = p.u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}
fs::path resolve(const fs::path &root, const std::string &relative) {
    if (relative.empty() || relative.size() > 240 || relative.find('\0') != std::string::npos)
        throw std::runtime_error("invalid relative path");
    auto p = fs::u8path(relative);
    if (p.is_absolute() || p.has_root_name() || relative.find(':') != std::string::npos)
        throw std::runtime_error("absolute paths are not allowed");
    for (auto &part : p)
        if (part == "..")
            throw std::runtime_error("parent paths are not allowed");
    auto base = fs::weakly_canonical(root), full = fs::weakly_canonical(base / p);
    auto b = base.begin(), f = full.begin();
    for (; b != base.end(); ++b, ++f)
        if (f == full.end() || _wcsicmp(b->c_str(), f->c_str()))
            throw std::runtime_error("path escapes game folder");
    return full;
}
static std::string read(const fs::path &p, size_t &remaining) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("cannot read " + utf8(p));
    auto n = in.tellg();
    if (n < 0 || size_t(n) > remaining)
        throw std::runtime_error("source and includes exceed 64 KiB");
    remaining -= size_t(n);
    std::string s(size_t(n), '\0');
    in.seekg(0);
    if (!s.empty() && !in.read(&s[0], s.size()))
        throw std::runtime_error("source read failed");
    return s;
}
struct Includes : ID3DInclude {
    std::filesystem::path root, initial;
    size_t remaining;
    struct Block {
        std::string text;
        std::filesystem::path parent;
        unsigned depth;
    };
    std::map<const void *, std::unique_ptr<Block>> blocks;
    std::string error;
    Includes(fs::path r, fs::path i, size_t n) : root(r), initial(i), remaining(n) {}
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, const char *name, LPCVOID parent, LPCVOID *data,
                           UINT *bytes) override {
        try {
            auto it = blocks.find(parent);
            auto dir = it == blocks.end() ? initial : it->second->parent;
            unsigned depth = it == blocks.end() ? 1 : it->second->depth + 1;
            if (depth > 8)
                throw std::runtime_error("include depth exceeds 8");
            auto path = resolve(root, utf8((dir / fs::u8path(name)).lexically_relative(root)));
            auto b = std::make_unique<Block>();
            b->text = read(path, remaining);
            b->parent = path.parent_path();
            b->depth = depth;
            if (b->text.empty())
                b->text = " ";
            *data = b->text.data();
            *bytes = UINT(b->text.size());
            blocks[*data] = std::move(b);
            return S_OK;
        } catch (const std::exception &e) {
            error = e.what();
            return E_FAIL;
        }
    }
    HRESULT __stdcall Close(LPCVOID data) override {
        blocks.erase(data);
        return S_OK;
    }
};
bool validate(const std::vector<unsigned> &code, bool vertex, std::string &error) {
    if (code.size() < 2 || code.size() > 16384 || code.front() != (vertex ? 0xfffe0300u : 0xffff0300u)) {
        error = "expected bounded Shader Model 3 bytecode";
        return false;
    }
    size_t at = 1;
    unsigned count = 0;
    while (at < code.size()) {
        unsigned t = code[at], op = t & 65535;
        if (op == 65535) {
            if (at != code.size() - 1) {
                error = "early END";
                return false;
            }
            return true;
        }
        if (op == 65533) {
            error = "invalid opcode";
            return false;
        }
        size_t n = op == 65534 ? (t >> 16) & 32767 : (t >> 24) & 15;
        if (n > code.size() - at - 1) {
            error = "truncated bytecode";
            return false;
        }
        if (op != 65534 && ++count > 512) {
            error = "instruction-token budget exceeds 512";
            return false;
        }
        at += n + 1;
    }
    error = "missing END token";
    return false;
}
json reflect(const std::vector<unsigned> &code) {
    json out = json::array();
    size_t at = 1;
    while (at < code.size()) {
        unsigned t = code[at], op = t & 65535;
        if (op == 65535)
            break;
        size_t n = op == 65534 ? (t >> 16) & 32767 : (t >> 24) & 15;
        if (op == 65534 && n > 8 && code[at + 1] == 0x42415443) {
            const char *p = reinterpret_cast<const char *>(&code[at + 2]);
            size_t bytes = (n - 1) * 4;
            auto u32 = [&](size_t o) {
                unsigned v = 0;
                if (o > bytes || bytes - o < 4)
                    throw std::runtime_error("bad CTAB");
                memcpy(&v, p + o, 4);
                return v;
            };
            auto u16 = [&](size_t o) {
                unsigned short v = 0;
                if (o > bytes || bytes - o < 2)
                    throw std::runtime_error("bad CTAB");
                memcpy(&v, p + o, 2);
                return unsigned(v);
            };
            unsigned count = u32(12), info = u32(16);
            if (count > 256 || info > bytes || count > (bytes - info) / 20)
                throw std::runtime_error("bad CTAB count/offset");
            size_t names = 0;
            for (unsigned i = 0; i < count; ++i) {
                size_t o = info + i * 20;
                unsigned name = u32(o), type = u32(o + 12);
                if (name >= bytes)
                    throw std::runtime_error("bad CTAB name");
                auto end = static_cast<const char *>(memchr(p + name, 0, bytes - name));
                if (!end)
                    throw std::runtime_error("unterminated CTAB name");
                names += size_t(end - (p + name));
                if (names > 65536 || type > bytes || bytes - type < 16)
                    throw std::runtime_error("invalid CTAB type/name bounds");
                out.push_back({{"name", std::string(p + name, end)},
                               {"set", u16(o + 4)},
                               {"register", u16(o + 6)},
                               {"count", u16(o + 8)},
                               {"class", u16(type)},
                               {"type", u16(type + 2)},
                               {"rows", u16(type + 4)},
                               {"columns", u16(type + 6)},
                               {"elements", u16(type + 8)}});
            }
            break;
        }
        at += n + 1;
    }
    return out;
}
Result compile(const json &opt, const fs::path &root) {
    Result result;
    ID3DBlob *blob = nullptr, *errors = nullptr;
    try {
        using Fn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                     LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
        using Asm = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, UINT,
                                      ID3DBlob **, ID3DBlob **);
        static std::once_flag once;
        static Fn fn = nullptr;
        static Asm assemble = nullptr;
        std::call_once(once, [] {
            auto m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (m) {
                fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3DCompile"));
                assemble = reinterpret_cast<Asm>(GetProcAddress(m, "D3DAssemble"));
            }
        });
        if (!fn)
            throw std::runtime_error("Microsoft d3dcompiler_47.dll is unavailable");
        auto profile = opt.value("profile", std::string("ps_3_0")),
             entry = opt.value("entry", std::string("main"));
        if (profile != "ps_3_0" && profile != "vs_3_0")
            throw std::runtime_error("profile must be ps_3_0 or vs_3_0");
        if (entry.empty() || entry.size() > 64 ||
            entry.find_first_not_of("_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") !=
                std::string::npos)
            throw std::runtime_error("invalid entry point");
        size_t remaining = 65536;
        std::string src, name = "<lua>";
        auto initial = root;
        if (opt.contains("source")) {
            src = opt.at("source").get<std::string>();
            if (src.size() > remaining)
                throw std::runtime_error("source exceeds 64 KiB");
            remaining -= src.size();
        } else {
            auto path = resolve(root, opt.at("path").get<std::string>());
            initial = path.parent_path();
            name = utf8(path);
            src = read(path, remaining);
        }
        Includes includes(fs::weakly_canonical(root), fs::weakly_canonical(initial), remaining);
        std::vector<std::pair<std::string, std::string>> values;
        auto macros = opt.value("defines", json::object());
        if (!macros.is_object() || macros.size() > 32)
            throw std::runtime_error("defines must be an object with at most 32 entries");
        for (auto it = macros.begin(); it != macros.end(); ++it) {
            auto v = it.value().get<std::string>();
            if (it.key().size() > 64 || v.size() > 256)
                throw std::runtime_error("macro too long");
            values.emplace_back(it.key(), v);
        }
        std::vector<D3D_SHADER_MACRO> defines;
        for (auto &v : values)
            defines.push_back({v.first.c_str(), v.second.c_str()});
        defines.push_back({nullptr, nullptr});
        auto language = opt.value("language", std::string("hlsl"));
        if (language != "hlsl" && language != "asm")
            throw std::runtime_error("language must be hlsl or asm");
        if (language == "asm" && !assemble)
            throw std::runtime_error("D3DAssemble unavailable");
        int optimization = opt.value("optimization", 3);
        if (optimization < 0 || optimization > 3)
            throw std::runtime_error("optimization must be 0..3");
        UINT flags = optimization == 0 ? 0x4000 : optimization == 1 ? 0 : optimization == 2 ? 0xc000 : 0x8000;
        if (opt.value("strict", false))
            flags |= 0x800;
        if (opt.value("debug", false))
            flags |= 1;
        if (opt.value("row_major", false))
            flags |= 8;
        if (opt.value("warnings_as_errors", false))
            flags |= 0x40000;
        HRESULT hr = language == "asm" ? assemble(src.data(), src.size(), name.c_str(), defines.data(),
                                                  &includes, 0, &blob, &errors)
                                       : fn(src.data(), src.size(), name.c_str(), defines.data(), &includes,
                                            entry.c_str(), profile.c_str(), flags, 0, &blob, &errors);
        if (errors) {
            result.error.assign(static_cast<const char *>(errors->GetBufferPointer()),
                                errors->GetBufferSize());
            if (result.error.size() > 16384)
                result.error.resize(16384);
        }
        if (FAILED(hr) || !blob) {
            if (!includes.error.empty())
                result.error += "\n" + includes.error;
            throw std::runtime_error(result.error.empty() ? "HLSL compilation failed" : result.error);
        }
        result.warnings = result.error;
        result.error.clear();
        if (blob->GetBufferSize() % 4 || blob->GetBufferSize() > 65536)
            throw std::runtime_error("invalid compiled size");
        result.code.resize(blob->GetBufferSize() / 4);
        memcpy(result.code.data(), blob->GetBufferPointer(), blob->GetBufferSize());
        if (!validate(result.code, profile == "vs_3_0", result.error))
            throw std::runtime_error(result.error);
        result.constants = reflect(result.code);
        result.error.clear();
    } catch (const std::exception &e) {
        result.code.clear();
        result.error = e.what();
    }
    if (blob)
        blob->Release();
    if (errors)
        errors->Release();
    return result;
}
} // namespace shader6
