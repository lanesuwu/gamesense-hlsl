#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fx9.h"
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <d3dx9.h>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
namespace fx9 {
using J = shader6::json;
namespace fs = std::filesystem;
namespace {
using Compiler = HRESULT(WINAPI *)(LPCSTR, UINT, const D3DXMACRO *, ID3DXInclude *, DWORD,
                                   ID3DXEffectCompiler **, ID3DXBuffer **);
using Create = HRESULT(WINAPI *)(IDirect3DDevice9 *, LPCVOID, UINT, const D3DXMACRO *, ID3DXInclude *, DWORD,
                                 ID3DXEffectPool *, ID3DXEffect **, ID3DXBuffer **);
Compiler compiler = nullptr;
Create create = nullptr;
void check(HRESULT hr, const char *what) {
    if (FAILED(hr)) {
        char s[180];
        sprintf_s(s, "%s failed (0x%08x)", what, unsigned(hr));
        throw std::runtime_error(s);
    }
}
void need(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}
template <class T> void release(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
std::string text(ID3DXBuffer *b) {
    if (!b)
        return {};
    auto size = std::min<size_t>(b->GetBufferSize(), 16384);
    std::string s(static_cast<const char *>(b->GetBufferPointer()), size);
    while (!s.empty() && !s.back())
        s.pop_back();
    return s;
}
std::string read(const fs::path &p, size_t &budget) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("cannot read FX source/include");
    auto n = in.tellg();
    need(n >= 0 && size_t(n) <= budget, "FX source and includes exceed 64 KiB");
    budget -= size_t(n);
    std::string s(size_t(n), '\0');
    in.seekg(0);
    if (!s.empty() && !in.read(&s[0], s.size()))
        throw std::runtime_error("FX source read failed");
    return s;
}
std::string utf8(const fs::path &p) {
    auto s = p.u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}
struct Include : ID3DXInclude {
    struct Block {
        std::string data;
        fs::path parent;
        unsigned depth;
    };
    fs::path root, initial;
    size_t budget;
    std::map<const void *, std::unique_ptr<Block>> blocks;
    std::string error;
    Include(fs::path r, fs::path i, size_t n) : root(r), initial(i), budget(n) {}
    HRESULT __stdcall Open(D3DXINCLUDE_TYPE, LPCSTR name, LPCVOID parent, LPCVOID *data,
                           UINT *size) override {
        try {
            auto it = blocks.find(parent);
            auto dir = it == blocks.end() ? initial : it->second->parent;
            unsigned depth = it == blocks.end() ? 1 : it->second->depth + 1;
            need(depth <= 8, "FX include depth exceeds 8");
            auto path = shader6::resolve(root, utf8((dir / fs::u8path(name)).lexically_relative(root)));
            auto b = std::make_unique<Block>();
            b->data = read(path, budget);
            b->parent = path.parent_path();
            b->depth = depth;
            if (b->data.empty())
                b->data = " ";
            *data = b->data.data();
            *size = UINT(b->data.size());
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
void flatten(const J &value, std::vector<J> &out, unsigned depth = 0) {
    need(depth < 8 && out.size() <= 16384, "FX parameter nesting/size limit");
    if (value.is_array()) {
        for (auto &x : value)
            flatten(x, out, depth + 1);
    } else
        out.push_back(value);
}
} // namespace
bool available() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto m = LoadLibraryExW(L"d3dx9_43.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m) {
            compiler = reinterpret_cast<Compiler>(GetProcAddress(m, "D3DXCreateEffectCompiler"));
            create = reinterpret_cast<Create>(GetProcAddress(m, "D3DXCreateEffect"));
        }
    });
    return compiler && create;
}
shader6::Result compile(const J &opt, const fs::path &root) {
    shader6::Result out;
    ID3DXEffectCompiler *object = nullptr;
    ID3DXBuffer *code = nullptr, *errors = nullptr;
    try {
        need(available(), "legacy FX requires Microsoft d3dx9_43.dll in the Windows system directory");
        size_t budget = 65536;
        auto initial = root;
        std::string source;
        if (opt.contains("source")) {
            source = opt.at("source").get<std::string>();
            need(source.size() <= budget, "FX source exceeds 64 KiB");
            budget -= source.size();
        } else {
            auto p = shader6::resolve(root, opt.at("path").get<std::string>());
            initial = p.parent_path();
            source = read(p, budget);
        }
        Include includes(fs::weakly_canonical(root), fs::weakly_canonical(initial), budget);
        std::vector<std::pair<std::string, std::string>> names;
        auto defines = opt.value("defines", J::object());
        need(defines.is_object() && defines.size() <= 32, "FX defines must contain at most 32 macros");
        for (auto it = defines.begin(); it != defines.end(); ++it) {
            auto v = it.value().get<std::string>();
            need(it.key().size() <= 64 && v.size() <= 256, "FX macro too long");
            names.emplace_back(it.key(), v);
        }
        std::vector<D3DXMACRO> macros;
        for (auto &n : names)
            macros.push_back({n.first.c_str(), n.second.c_str()});
        macros.push_back({nullptr, nullptr});
        int level = opt.value("optimization", 3);
        need(level >= 0 && level <= 3, "FX optimization must be 0..3");
        DWORD flags = level == 0 ? 0x4000 : level == 1 ? 0 : level == 2 ? 0xc000 : 0x8000;
        if (opt.value("strict", false))
            flags |= 0x800;
        if (opt.value("debug", false))
            flags |= 1;
        if (opt.value("row_major", false))
            flags |= 8;
        HRESULT hr =
            compiler(source.data(), UINT(source.size()), macros.data(), &includes, flags, &object, &errors);
        out.error = text(errors);
        release(errors);
        if (FAILED(hr) || !object)
            throw std::runtime_error(out.error.empty() ? "FX parse failed" : out.error);
        hr = object->CompileEffect(flags, &code, &errors);
        auto diagnostic = text(errors);
        if (!diagnostic.empty())
            out.error = diagnostic;
        if (FAILED(hr) || !code)
            throw std::runtime_error(out.error.empty() ? "FX compilation failed" : out.error);
        if (opt.value("warnings_as_errors", false) && !out.error.empty())
            throw std::runtime_error(out.error);
        need(code->GetBufferSize() && code->GetBufferSize() % 4 == 0 && code->GetBufferSize() <= 1024 * 1024,
             "FX compiled size exceeds 1 MiB or is misaligned");
        out.code.resize(code->GetBufferSize() / 4);
        memcpy(out.code.data(), code->GetBufferPointer(), code->GetBufferSize());
        out.warnings = out.error;
        out.error.clear();
    } catch (const std::exception &e) {
        out.code.clear();
        out.error = e.what();
    }
    release(errors);
    release(code);
    release(object);
    return out;
}
struct Effect::Impl {
    ID3DXEffect *object = nullptr;
    bool begun = false, pass = false;
    std::vector<D3DXHANDLE> textureHandles;
    ~Impl() {
        if (pass)
            object->EndPass();
        if (begun)
            object->End();
        release(object);
    }
    D3DXHANDLE technique(const std::string &name) const {
        D3DXHANDLE h = nullptr;
        if (!name.empty())
            h = object->GetTechniqueByName(name.c_str());
        else {
            D3DXEFFECT_DESC d{};
            check(object->GetDesc(&d), "FX GetDesc");
            for (unsigned i = 0; i < d.Techniques; ++i) {
                auto t = object->GetTechnique(i);
                if (SUCCEEDED(object->ValidateTechnique(t))) {
                    h = t;
                    break;
                }
            }
        }
        need(h != nullptr, "FX technique not found");
        check(object->ValidateTechnique(h), "FX ValidateTechnique");
        return h;
    }
    void cleanup() noexcept {
        if (pass) {
            object->EndPass();
            pass = false;
        }
        if (begun) {
            object->End();
            begun = false;
        }
        for (auto h : textureHandles)
            object->SetTexture(h, nullptr);
        textureHandles.clear();
    }
    void parameter(D3DXHANDLE h, const J &value,
                   const std::function<IDirect3DBaseTexture9 *(const J &)> &resolve, unsigned depth = 0) {
        need(depth < 8 && h, "FX parameter not found or nested too deeply");
        D3DXPARAMETER_DESC d{};
        check(object->GetParameterDesc(h, &d), "FX parameter description");
        if (value.is_object() && value.contains("raw_hex")) {
            need((d.Type == D3DXPT_FLOAT || d.Type == D3DXPT_INT || d.Type == D3DXPT_BOOL) &&
                     d.Class != D3DXPC_OBJECT && d.Class != D3DXPC_STRUCT,
                 "FX raw bytes are restricted to numeric parameters");
            auto s = value["raw_hex"].get<std::string>();
            need(d.Bytes <= 65536, "FX raw parameter size limit");
            need(s.size() == size_t(d.Bytes) * 2 && s.size() <= 131072,
                 "FX raw parameter byte count mismatch");
            std::vector<BYTE> b(d.Bytes);
            auto digit = [](char c) {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            for (unsigned i = 0; i < d.Bytes; ++i) {
                int a = digit(s[i * 2]), v = digit(s[i * 2 + 1]);
                need(a >= 0 && v >= 0, "invalid FX raw hex");
                b[i] = BYTE(a * 16 + v);
            }
            check(object->SetValue(h, b.data(), d.Bytes), "FX raw SetValue");
            return;
        }
        if (d.Elements && value.is_array() && !value.empty() && value[0].is_object()) {
            need(value.size() == d.Elements, "FX object array size mismatch");
            for (unsigned i = 0; i < d.Elements; ++i)
                parameter(object->GetParameterElement(h, i), value[i], resolve, depth + 1);
            return;
        }
        if (d.Class == D3DXPC_STRUCT) {
            need(value.is_object(), "FX struct parameters require an object");
            for (auto it = value.begin(); it != value.end(); ++it)
                parameter(object->GetParameterByName(h, it.key().c_str()), it.value(), resolve, depth + 1);
            return;
        }
        if (d.Type == D3DXPT_TEXTURE || d.Type == D3DXPT_TEXTURE1D || d.Type == D3DXPT_TEXTURE2D ||
            d.Type == D3DXPT_TEXTURE3D || d.Type == D3DXPT_TEXTURECUBE) {
            need(value.is_object(), "FX texture parameter requires {texture=handle} or {capture=true}");
            check(object->SetTexture(h, resolve(value)), "FX SetTexture");
            textureHandles.push_back(h);
            return;
        }
        if (d.Type == D3DXPT_STRING) {
            auto s = value.get<std::string>();
            need(s.size() <= 1024, "FX string too long");
            check(object->SetString(h, s.c_str()), "FX SetString");
            return;
        }
        std::vector<J> flat;
        flatten(value, flat);
        uint64_t count = uint64_t(std::max(1u, d.Elements)) * d.Rows * d.Columns;
        need(d.Rows && d.Columns && flat.size() == count && count <= 16384,
             "FX numeric parameter shape mismatch");
        if (d.Type == D3DXPT_FLOAT) {
            std::vector<float> v;
            for (auto &x : flat) {
                need(x.is_number(), "FX float required");
                double n = x.get<double>();
                need(std::isfinite(n) && std::abs(n) <= 3.4028234663852886e38, "FX float32 range");
                v.push_back(float(n));
            }
            if (d.Class == D3DXPC_MATRIX_ROWS || d.Class == D3DXPC_MATRIX_COLUMNS) {
                need(d.Rows <= 4 && d.Columns <= 4, "FX matrix shape");
                std::vector<D3DXMATRIX> matrices(std::max(1u, d.Elements));
                memset(matrices.data(), 0, matrices.size() * sizeof(D3DXMATRIX));
                for (unsigned e = 0; e < matrices.size(); ++e)
                    for (unsigned row = 0; row < d.Rows; ++row)
                        for (unsigned col = 0; col < d.Columns; ++col)
                            matrices[e].m[row][col] = v[(e * d.Rows + row) * d.Columns + col];
                check(d.Elements ? object->SetMatrixArray(h, matrices.data(), UINT(matrices.size()))
                                 : object->SetMatrix(h, matrices.data()),
                      "FX SetMatrix");
            } else if (d.Class == D3DXPC_VECTOR) {
                need(d.Rows == 1 && d.Columns <= 4, "FX vector shape");
                std::vector<D3DXVECTOR4> vectors(std::max(1u, d.Elements));
                for (unsigned e = 0; e < vectors.size(); ++e) {
                    auto f = [&](unsigned c) { return c < d.Columns ? v[e * d.Columns + c] : 0.f; };
                    vectors[e] = D3DXVECTOR4(f(0), f(1), f(2), f(3));
                }
                check(d.Elements ? object->SetVectorArray(h, vectors.data(), UINT(vectors.size()))
                                 : object->SetVector(h, vectors.data()),
                      "FX SetVector");
            } else if (!d.Elements && v.size() == 1)
                check(object->SetFloat(h, v[0]), "FX SetFloat");
            else
                check(object->SetFloatArray(h, v.data(), UINT(v.size())), "FX SetFloatArray");
        } else if (d.Type == D3DXPT_INT) {
            std::vector<int> v;
            for (auto &x : flat) {
                need(x.is_number_integer(), "FX integer required");
                auto n = x.get<int64_t>();
                need(n >= INT_MIN && n <= INT_MAX, "FX int32 range");
                v.push_back(int(n));
            }
            if (!d.Elements && v.size() == 1)
                check(object->SetInt(h, v[0]), "FX SetInt");
            else if (!d.Elements) {
                need(d.Bytes == v.size() * 4, "FX int vector bytes");
                check(object->SetValue(h, v.data(), d.Bytes), "FX int vector");
            } else
                check(object->SetIntArray(h, v.data(), UINT(v.size())), "FX SetIntArray");
        } else if (d.Type == D3DXPT_BOOL) {
            std::vector<BOOL> v;
            for (auto &x : flat) {
                need(x.is_boolean(), "FX boolean required");
                v.push_back(x.get<bool>());
            }
            if (!d.Elements && v.size() == 1)
                check(object->SetBool(h, v[0]), "FX SetBool");
            else if (!d.Elements) {
                need(d.Bytes == v.size() * 4, "FX bool vector bytes");
                check(object->SetValue(h, v.data(), d.Bytes), "FX bool vector");
            } else
                check(object->SetBoolArray(h, v.data(), UINT(v.size())), "FX SetBoolArray");
        } else
            throw std::runtime_error("FX parameter type is not settable by this value");
    }
};
Effect::Effect(IDirect3DDevice9 *d, const std::vector<unsigned> &code) : impl(new Impl) {
    need(available(), "D3DX9 FX runtime unavailable");
    ID3DXBuffer *error = nullptr;
    HRESULT hr = create(d, code.data(), UINT(code.size() * 4), nullptr, nullptr, D3DXFX_LARGEADDRESSAWARE,
                        nullptr, &impl->object, &error);
    std::string message = text(error);
    release(error);
    if (FAILED(hr))
        throw std::runtime_error(message.empty() ? "FX GPU creation failed" : message);
}
Effect::~Effect() = default;
J Effect::describe() const {
    J params = J::array(), techniques = J::array();
    D3DXEFFECT_DESC d{};
    check(impl->object->GetDesc(&d), "FX GetDesc");
    need(d.Parameters <= 256 && d.Techniques <= 128, "FX parameter/technique limit");
    for (unsigned i = 0; i < d.Parameters; ++i) {
        D3DXPARAMETER_DESC p{};
        check(impl->object->GetParameterDesc(impl->object->GetParameter(nullptr, i), &p),
              "FX parameter description");
        params.push_back({{"name", p.Name ? p.Name : ""},
                          {"semantic", p.Semantic ? p.Semantic : ""},
                          {"annotations", p.Annotations},
                          {"class", unsigned(p.Class)},
                          {"type", unsigned(p.Type)},
                          {"rows", p.Rows},
                          {"columns", p.Columns},
                          {"elements", p.Elements},
                          {"members", p.StructMembers},
                          {"bytes", p.Bytes}});
    }
    for (unsigned i = 0; i < d.Techniques; ++i) {
        auto h = impl->object->GetTechnique(i);
        D3DXTECHNIQUE_DESC t{};
        check(impl->object->GetTechniqueDesc(h, &t), "FX technique description");
        need(t.Passes <= 128, "FX technique exceeds 128 passes");
        J passes = J::array();
        for (unsigned n = 0; n < t.Passes; ++n) {
            D3DXPASS_DESC p{};
            check(impl->object->GetPassDesc(impl->object->GetPass(h, n), &p), "FX pass description");
            passes.push_back(p.Name ? p.Name : "");
        }
        techniques.push_back({{"name", t.Name ? t.Name : ""},
                              {"passes", passes},
                              {"valid", SUCCEEDED(impl->object->ValidateTechnique(h))}});
    }
    return {{"parameters", params}, {"techniques", techniques}};
}
unsigned Effect::passes(const std::string &name) const {
    D3DXTECHNIQUE_DESC d{};
    check(impl->object->GetTechniqueDesc(impl->technique(name), &d), "FX technique description");
    need(d.Passes && d.Passes <= 128, "FX pass count limit");
    return d.Passes;
}
void Effect::start(const std::string &name, unsigned pass, const J &parameters,
                   const std::function<IDirect3DBaseTexture9 *(const J &)> &resolve) {
    impl->cleanup();
    try {
        need(parameters.is_object() || parameters.empty(), "FX parameters must be an object");
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            impl->parameter(impl->object->GetParameterByName(nullptr, it.key().c_str()), it.value(), resolve);
        check(impl->object->SetTechnique(impl->technique(name)), "FX SetTechnique");
        UINT count = 0;
        check(impl->object->Begin(&count, 0), "FX Begin");
        impl->begun = true;
        need(pass < count, "FX pass index out of range");
        check(impl->object->BeginPass(pass), "FX BeginPass");
        impl->pass = true;
    } catch (...) {
        impl->cleanup();
        throw;
    }
}
void Effect::end() noexcept {
    impl->cleanup();
}
void Effect::next(unsigned pass) {
    need(impl->begun && impl->pass, "FX technique is not active");
    check(impl->object->EndPass(), "FX EndPass");
    impl->pass = false;
    check(impl->object->BeginPass(pass), "FX BeginPass");
    impl->pass = true;
}
} // namespace fx9
