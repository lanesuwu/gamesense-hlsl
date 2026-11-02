// Generic DX9 resource and pass engine. Compile results become usable only
// after render-thread installation; preserve owner/generation/reset checks.
#include "dx9_api.h"
#include "compiler6.h"
#include "dds_loader.h"
#include "fx9.h"
#include "hlsl_renderer.h"
#include "hud_chat.h"
#include "profile9.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
namespace dx9_api {
namespace {
using J = json;
std::atomic<unsigned> backendBusyRetries{0};
template <class T> void drop(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void checked(HRESULT h, const char *action) {
    if (FAILED(h)) {
        char msg[160];
        sprintf_s(msg, "%s failed: 0x%08x", action, unsigned(h));
        throw std::runtime_error(msg);
    }
}
unsigned number(const J &j, const char *key, unsigned def, unsigned max) {
    auto v = j.value(key, J(def));
    require(v.is_number_integer(), "expected integer");
    auto n = v.get<int64_t>();
    require(n >= 0 && n <= max, "integer outside API limits");
    return unsigned(n);
}
float scalar(const J &j) {
    require(j.is_number(), "expected number");
    double n = j.get<double>();
    require(std::isfinite(n) && std::abs(n) <= 3.4028234663852886e38, "number outside finite float32 range");
    return float(n);
}
std::vector<float> floats(const J &j, size_t max) {
    require(j.is_array() && j.size() <= max, "invalid float array size");
    std::vector<float> v;
    v.reserve(j.size());
    for (auto &x : j)
        v.push_back(scalar(x));
    return v;
}
std::vector<BYTE> decodeHex(const std::string &s, size_t max) {
    require(s.size() % 2 == 0 && s.size() / 2 <= max, "invalid byte string size");
    std::vector<BYTE> b(s.size() / 2);
    auto digit = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < b.size(); ++i) {
        int a = digit(s[i * 2]), c = digit(s[i * 2 + 1]);
        require(a >= 0 && c >= 0, "invalid hex byte");
        b[i] = BYTE(a * 16 + c);
    }
    return b;
}
std::string hex(const std::vector<BYTE> &b) {
    std::string s(b.size() * 2, '0');
    const char *d = "0123456789abcdef";
    for (size_t i = 0; i < b.size(); ++i) {
        s[2 * i] = d[b[i] >> 4];
        s[2 * i + 1] = d[b[i] & 15];
    }
    return s;
}
const J emptyArray = J::array(), emptyObject = J::object();
const J &field(const J &object, const char *name, const J &fallback) {
    require(object.is_object(), "expected object");
    const auto it = object.find(name);
    return it == object.end() ? fallback : *it;
}
static const std::pair<D3DRENDERSTATETYPE, DWORD> renderBaseline[] = {
    {D3DRS_ZENABLE, FALSE},           {D3DRS_ZWRITEENABLE, FALSE},        {D3DRS_STENCILENABLE, FALSE},
    {D3DRS_ALPHATESTENABLE, FALSE},   {D3DRS_CULLMODE, D3DCULL_NONE},     {D3DRS_ALPHABLENDENABLE, FALSE},
    {D3DRS_SCISSORTESTENABLE, FALSE}, {D3DRS_SRGBWRITEENABLE, FALSE},     {D3DRS_FOGENABLE, FALSE},
    {D3DRS_LIGHTING, FALSE},          {D3DRS_FILLMODE, D3DFILL_SOLID},    {D3DRS_COLORWRITEENABLE, 15},
    {D3DRS_CLIPPLANEENABLE, 0},       {D3DRS_MULTISAMPLEMASK, 0xffffffff}};
static const std::pair<D3DRENDERSTATETYPE, DWORD> geometryBaseline[] = {
    {D3DRS_ZFUNC, D3DCMP_LESSEQUAL},
    {D3DRS_STENCILFUNC, D3DCMP_ALWAYS},
    {D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP},
    {D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP},
    {D3DRS_STENCILPASS, D3DSTENCILOP_KEEP},
    {D3DRS_STENCILREF, 0},
    {D3DRS_STENCILMASK, 0xffffffff},
    {D3DRS_STENCILWRITEMASK, 0xffffffff},
    {D3DRS_TWOSIDEDSTENCILMODE, FALSE},
    {D3DRS_ALPHAFUNC, D3DCMP_GREATER},
    {D3DRS_ALPHAREF, 0},
    {D3DRS_POINTSIZE, 0x3f800000},
    {D3DRS_POINTSPRITEENABLE, FALSE},
    {D3DRS_POINTSCALEENABLE, FALSE},
    {D3DRS_ANTIALIASEDLINEENABLE, FALSE},
    {D3DRS_DEPTHBIAS, 0},
    {D3DRS_SLOPESCALEDEPTHBIAS, 0}};
static const std::pair<D3DSAMPLERSTATETYPE, DWORD> samplerBaseline[] = {
    {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
    {D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP}, {D3DSAMP_MINFILTER, D3DTEXF_POINT},
    {D3DSAMP_MAGFILTER, D3DTEXF_POINT},    {D3DSAMP_MIPFILTER, D3DTEXF_NONE},
    {D3DSAMP_SRGBTEXTURE, FALSE}};
// Resource snapshots and compiler-worker handoff. GPU objects stay render-thread owned.
struct Resource {
    unsigned id = 0, owner = 0, generation = 0, installedGeneration = 0;
    std::string kind, state = "awaiting_gpu", jobState = "awaiting_gpu", error, warnings, profile = "ps_3_0";
    J config, constants = J::array(), pendingConstants = J::array(), effectMeta = J::object();
    std::unique_ptr<fx9::Effect> effect;
    bool program() const {
        return kind == "shader" || kind == "effect";
    }
    std::vector<BYTE> bytes;
    std::vector<unsigned> code, pendingCode;
    bool dirty = false, defined = false;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DTexture9 *texture = nullptr;
    IDirect3DCubeTexture9 *cube = nullptr;
    IDirect3DVolumeTexture9 *volume = nullptr;
    IDirect3DSurface9 *surface = nullptr, *depth = nullptr;
    IDirect3DVertexBuffer9 *vb = nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    std::vector<D3DVERTEXELEMENT9> elements;
    std::array<unsigned, 3> panelTextures{};
    bool disabled = false;
    double ms = 0;
    unsigned over = 0;
    size_t gpuBytes = 0;
    IDirect3DBaseTexture9 *base() {
        if (cube)
            return cube;
        if (volume)
            return volume;
        return texture;
    }
    void gpu() {
        effect.reset();
        drop(ps);
        drop(vs);
        drop(texture);
        drop(cube);
        drop(volume);
        drop(surface);
        drop(depth);
        drop(vb);
        drop(ib);
        drop(decl);
        gpuBytes = 0;
    }
};
struct Completed {
    unsigned id, gen;
    std::string kind;
    shader6::Result result;
    dds::Image image;
};
struct Work {
    unsigned id, gen;
    J opt;
    std::string kind;
    std::filesystem::path root;
};
// Query objects are pooled only after the complete timing sample has been consumed.
struct TimingDraw {
    unsigned id, vertex = 0;
    IDirect3DQuery9 *start = nullptr, *end = nullptr;
};
struct TimingFrame {
    IDirect3DQuery9 *frequency = nullptr, *disjoint = nullptr;
    std::vector<TimingDraw> draws;
    size_t used = 0;
    bool valid = true, profiled = false, copyMeasured = false;
    unsigned profileEpoch = 0;
    IDirect3DQuery9 *wholeStart = nullptr, *wholeEnd = nullptr, *copyEnd = nullptr;
};
// Immutable prepared layouts are shared; each accepted frame owns its changing values.
struct UniformBlock {
    unsigned reg = 0, offset = 0, count = 0;
    bool vertex = false;
};
struct PreparedPass {
    std::vector<UniformBlock> uniforms;
};
struct PreparedBatch {
    J items;
    std::vector<PreparedPass> passes;
    unsigned count = 0;
    size_t tail = 0, bytes = 0;
};
struct PreparedSubmission {
    std::shared_ptr<const PreparedBatch> batch;
    std::vector<float> values;
};
struct Owner {
    J frame, presentation;
    std::map<unsigned, std::shared_ptr<const PreparedBatch>> templates;
    std::shared_ptr<const PreparedSubmission> preparedFrame, preparedPresentation;
    unsigned preparedSubmissions = 0, jsonSubmissions = 0, rawUpdates = 0, jsonUpdates = 0;
    bool profiling = false;
    ULONGLONG submitted = 0;
    double budget = 2;
    unsigned sequence = 0, completedSequence = 0, retainMs = 0, retainedDraws = 0;
    std::string error;
    J readbacks = J::object();
};
thread_local HRESULT restoreFailure = S_OK;
void restored(HRESULT hr) {
    if (FAILED(hr) && SUCCEEDED(restoreFailure))
        restoreFailure = hr;
}
// A cached state block is leased by one guard at a time. Nested guards own a
// temporary block so they cannot overwrite the outer snapshot.
struct StateCache {
    IDirect3DStateBlock9 *block = nullptr;
    bool leased = false;
    StateCache() = default;
    StateCache(const StateCache &) = delete;
    ~StateCache() {
        reset();
    }
    unsigned created = 0, reused = 0;
    void reset() {
        drop(block);
    }
};
struct State {
    IDirect3DDevice9 *d;
    IDirect3DStateBlock9 *block = nullptr;
    IDirect3DSurface9 *rt[4]{}, *depth = nullptr;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    unsigned n = 1, streams = 0;
    UINT frequencies[16]{};
    StateCache *cache = nullptr;
    State(IDirect3DDevice9 *device, StateCache &storage, const D3DCAPS9 &caps) : d(device) {
        profile9::Scope measured(profile9::StateSave);
        n = std::min(4u, unsigned(caps.NumSimultaneousRTs));
        streams = std::min(16u, unsigned(caps.MaxStreams));
        try {
            if (!storage.leased) {
                cache = &storage;
                storage.leased = true;
                if (storage.block) {
                    block = storage.block;
                    block->AddRef();
                    checked(block->Capture(), "Capture state");
                    ++storage.reused;
                } else {
                    // CreateStateBlock captures the current state itself.
                    checked(d->CreateStateBlock(D3DSBT_ALL, &block), "CreateStateBlock");
                    storage.block = block;
                    storage.block->AddRef();
                    ++storage.created;
                }
            } else {
                checked(d->CreateStateBlock(D3DSBT_ALL, &block), "Create nested state block");
                ++storage.created;
            }
            for (unsigned i = 0; i < streams; ++i)
                checked(d->GetStreamSourceFreq(i, &frequencies[i]), "Get stream frequency");
            for (unsigned i = 0; i < n; ++i) {
                const auto hr = d->GetRenderTarget(i, &rt[i]);
                if (hr != D3DERR_NOTFOUND)
                    checked(hr, "Get render target");
            }
            const auto hr = d->GetDepthStencilSurface(&depth);
            if (hr != D3DERR_NOTFOUND)
                checked(hr, "Get depth surface");
            checked(d->GetViewport(&viewport), "Get viewport");
            checked(d->GetScissorRect(&scissor), "Get scissor");
        } catch (...) {
            for (auto &surface : rt)
                drop(surface);
            drop(depth);
            drop(block);
            if (cache) {
                cache->reset();
                cache->leased = false;
            }
            throw;
        }
    }
    ~State() {
        profile9::Scope measured(profile9::StateRestore);
        restored(d->SetDepthStencilSurface(nullptr));
        for (unsigned i = 1; i < n; ++i)
            restored(d->SetRenderTarget(i, nullptr));
        for (unsigned i = 0; i < n; ++i) {
            restored(d->SetRenderTarget(i, rt[i]));
            drop(rt[i]);
        }
        restored(d->SetDepthStencilSurface(depth));
        drop(depth);
        if (block) {
            const auto hr = block->Apply();
            restored(hr);
            if (FAILED(hr) && cache)
                cache->reset();
            drop(block);
        }
        restored(d->SetViewport(&viewport));
        restored(d->SetScissorRect(&scissor));
        for (unsigned i = 0; i < streams; ++i)
            restored(d->SetStreamSourceFreq(i, frequencies[i]));
        if (cache)
            cache->leased = false;
    }
};
class Engine {
    std::mutex mutex, workMutex;
    std::condition_variable cv;
    std::thread worker;
    bool stopping = false;
    std::deque<Work> work;
    std::deque<Completed> finished;
    std::vector<unsigned> pendingClose;
    std::map<unsigned, Owner> owners;
    std::map<unsigned, std::unique_ptr<Resource>> resources;
    std::vector<std::unique_ptr<Resource>> retired;
    unsigned next = 1;
    HlslRenderer panels;
    StateCache stateCache;
    IDirect3DVertexDeclaration9 *quadDeclaration = nullptr;
    IDirect3DDevice9 *capsDevice = nullptr;
    std::filesystem::path root = std::filesystem::current_path();
    IDirect3DTexture9 *capture = nullptr;
    IDirect3DSurface9 *captureSurface = nullptr;
    UINT captureW = 0, captureH = 0;
    std::deque<TimingFrame> timings;
    std::vector<TimingFrame> timingPool;
    size_t openTiming = SIZE_MAX;
    unsigned queriesCreated = 0, queriesReused = 0, queryFailures = 0;
#ifdef HLSL_STANDALONE_TEST
    unsigned testQueryFailure = 0;
#endif
    unsigned bufferCreations = 0, bufferUploads = 0;
    uint64_t bufferUploadedBytes = 0, screenCaptures = 0, genericDrawCalls = 0, clearCalls = 0;
    TimingFrame activeTiming;
    bool timingSupported = false, timingProbed = false;
    D3DCAPS9 caps{};
    bool initialized = false, paused = false;
    unsigned frames = 0, resets = 0, profilingOwners = 0;
    uint64_t gpuSamples = 0, copySamples = 0;
    double gpuTotalMs = 0, gpuMaxMs = 0, copyTotalMs = 0, copyMaxMs = 0;
    size_t cpuUsed(unsigned except = 0) {
        size_t n = 0;
        for (auto &r : resources)
            if (r.first != except)
                n += r.second->bytes.size() + 4 * (r.second->code.size() + r.second->pendingCode.size());
        for (auto &r : retired)
            n += r->bytes.size() + 4 * (r->code.size() + r->pendingCode.size());
        return n;
    }
    // Resource ownership, compiler results and render-thread installation.
    void cpuBudget(size_t n, unsigned id) {
        require(n <= 64 * 1024 * 1024 && cpuUsed(id) + n <= 128 * 1024 * 1024,
                "retained CPU resource budget exceeded (128 MiB)");
    }
    Resource &get(unsigned owner, unsigned id, const char *kind = nullptr) {
        auto it = resources.find(id);
        require(it != resources.end() && it->second->owner == owner, "unknown or foreign resource handle");
        if (kind)
            require(it->second->kind == kind, "resource type mismatch");
        return *it->second;
    }
    J describe(Resource &r) {
        return {{"handle", r.id},
                {"kind", r.kind},
                {"state", r.program() ? r.jobState : r.state},
                {"gpu_state", r.state},
                {"generation", r.generation},
                {"installed_generation", r.installedGeneration},
                {"error", r.error},
                {"warnings", r.warnings},
                {"profile", r.profile},
                {"constants", r.constants},
                {"techniques", r.effectMeta.value("techniques", J::array())},
                {"usable", bool(r.ps || r.vs || r.effect || r.base() || r.vb || r.ib || r.decl)},
                {"gpu_ms", r.ms},
                {"disabled", r.disabled},
                {"defined", r.defined},
                {"bytes", r.bytes.size()},
                {"width", r.config.value("width", 0)},
                {"height", r.config.value("height", 0)}};
    }
    Resource &allocate(unsigned owner, const std::string &kind) {
        require(resources.size() + retired.size() < 256,
                "global resource limit (256), including pending releases");
        unsigned n = 0;
        for (auto &r : resources)
            if (r.second->owner == owner)
                ++n;
        require(n < 128, "owner resource limit (128)");
        require(next < 16777216, "resource handle space exhausted; restart session");
        auto r = std::make_unique<Resource>();
        r->id = next++;
        r->owner = owner;
        r->kind = kind;
        auto id = r->id;
        resources[id] = std::move(r);
        return *resources[id];
    }
    void queue(Resource &r, const J &opt, const std::string &kind) {
        std::lock_guard<std::mutex> lock(workMutex);
        require(work.size() < 32, "compile/asset queue is full");
        ++r.generation;
        r.pendingCode.clear();
        r.state = "queued";
        r.jobState = "queued";
        r.error.clear();
        work.push_back({r.id, r.generation, opt, kind, root});
        cv.notify_one();
    }
    void run() {
        for (;;) {
            Work w;
            {
                std::unique_lock<std::mutex> lock(workMutex);
                cv.wait(lock, [&] { return stopping || !work.empty(); });
                if (stopping)
                    return;
                w = std::move(work.front());
                work.pop_front();
            }
            {
                std::lock_guard<std::mutex> lock(workMutex);
                finished.push_back({w.id, w.gen, "started", {}, {}});
            }
            Completed c{w.id, w.gen, w.kind, {}, {}};
            try {
                if (w.kind == "shader")
                    c.result = shader6::compile(w.opt, w.root);
                else if (w.kind == "effect")
                    c.result = fx9::compile(w.opt, w.root);
                else {
                    auto p = shader6::resolve(w.root, w.opt.at("path").get<std::string>());
                    char why[256]{};
                    if (!dds::decode(p.c_str(), c.image, why, sizeof(why)))
                        c.result.error = why;
                }
            } catch (const std::exception &e) {
                c.result.error = e.what();
            }
            {
                std::lock_guard<std::mutex> lock(workMutex);
                finished.push_back(std::move(c));
            }
        }
    }
    void collect() {
        std::deque<Completed> done;
        {
            std::lock_guard<std::mutex> lock(workMutex);
            done.swap(finished);
        }
        for (auto &c : done) {
            auto it = resources.find(c.id);
            if (it == resources.end() || it->second->generation != c.gen)
                continue;
            auto &r = *it->second;
            if (c.kind == "started") {
                r.state = "compiling";
                r.jobState = "compiling";
                continue;
            }
            if (!c.result.error.empty()) {
                r.state = "failed";
                r.jobState = "failed";
                r.error = c.result.error;
                continue;
            }
            if (c.kind == "shader" || c.kind == "effect") {
                if (cpuUsed(r.id) + 4 * (r.code.size() + c.result.code.size()) > 128 * 1024 * 1024) {
                    r.state = "failed";
                    r.jobState = "failed";
                    r.error = "program payload exceeds CPU resource budget";
                    continue;
                }
                r.pendingCode = std::move(c.result.code);
                r.pendingConstants = c.result.constants;
                r.warnings = c.result.warnings;
            } else {
                size_t n = c.image.pixels.size() * 4;
                if (cpuUsed(r.id) + n > 128 * 1024 * 1024) {
                    r.state = "failed";
                    r.error = "retained CPU resource budget exceeded";
                    continue;
                }
                r.config["width"] = c.image.width;
                r.config["height"] = c.image.height;
                r.bytes.resize(n);
                memcpy(r.bytes.data(), c.image.pixels.data(), r.bytes.size());
            }
            r.state = "awaiting_gpu";
            r.jobState = "awaiting_gpu";
        }
    }
    void erase(unsigned owner, unsigned id) {
        auto &r = get(owner, id);
        ++r.generation;
        {
            std::lock_guard<std::mutex> lock(workMutex);
            work.erase(std::remove_if(work.begin(), work.end(), [&](const Work &w) { return w.id == id; }),
                       work.end());
        }
        retired.push_back(std::move(resources.at(id)));
        resources.erase(id);
    }
    void retire() {
        for (auto &r : retired) {
            panels.release_user(int(r->id));
            r->gpu();
        }
        retired.clear();
    }
    void closeOwner(unsigned owner) {
        auto it = owners.find(owner);
        if (it == owners.end())
            return;
        hud_chat::release(owner);
        std::vector<unsigned> ids;
        for (auto &r : resources)
            if (r.second->owner == owner)
                ids.push_back(r.first);
        for (auto id : ids)
            erase(owner, id);
        if (it->second.profiling && --profilingOwners == 0)
            profile9::enabled.store(false, std::memory_order_relaxed);
        owners.erase(it);
    }
    void flushCloses() {
        std::vector<unsigned> ids;
        {
            std::lock_guard<std::mutex> lock(workMutex);
            ids.swap(pendingClose);
        }
        for (auto id : ids)
            closeOwner(id);
    }
    void createGPU(IDirect3DDevice9 *d, Resource &r) {
        profile9::Scope measured(profile9::Upload);
        const bool dynamicBuffer =
            (r.kind == "vertices" || r.kind == "indices") && r.config.value("dynamic", false);
        const bool reuseBuffer = dynamicBuffer && (r.vb || r.ib);
        size_t cost = r.bytes.size();
        if (r.kind == "texture" || r.kind == "cube" || r.kind == "volume") {
            unsigned w = number(r.config, "width", 0, 2048), h = number(r.config, "height", w, 2048),
                     z = r.kind == "volume" ? number(r.config, "depth", w, 256) : 1,
                     levels = number(r.config, "levels", 1, 12);
            if (r.kind == "cube")
                h = w;
            auto f = r.config.value("format", std::string("rgba8"));
            unsigned bpp = f == "rgba32f" ? 16 : f == "rgba16f" ? 8 : 4;
            cost = 0;
            for (unsigned l = 0; l < levels; ++l)
                cost += size_t(std::max(1u, w >> l)) * std::max(1u, h >> l) * std::max(1u, z >> l) * bpp *
                        (r.kind == "cube" ? 6 : 1);
        }
        if (r.program())
            cost = 8 * (r.pendingCode.empty() ? r.code.size() : r.pendingCode.size());
        if (r.kind == "target") {
            size_t w = number(r.config, "width", 0, 2048), h = number(r.config, "height", 0, 2048);
            auto f = r.config.value("format", std::string("rgba8"));
            cost = w * h * (f == "rgba32f" ? 16 : f == "rgba16f" ? 8 : 4);
            if (r.config.value("depth", false))
                cost += w * h * 4;
        }
        size_t used = 0;
        for (auto &v : resources)
            used += v.second->gpuBytes;
        require(used + (reuseBuffer ? 0 : cost) <= 256 * 1024 * 1024,
                "GPU resource budget exceeded (256 MiB), including replacement allocation");
        if (r.kind == "effect") {
            bool replacement = !r.pendingCode.empty();
            auto &code = replacement ? r.pendingCode : r.code;
            if (code.empty())
                return;
            auto next = std::make_unique<fx9::Effect>(d, code);
            auto meta = next->describe();
            r.effect = std::move(next);
            r.effectMeta = meta;
            r.constants = meta["parameters"];
            if (replacement) {
                r.code = std::move(r.pendingCode);
                r.installedGeneration = r.generation;
                r.jobState = "ready";
                r.error.clear();
            }
            r.disabled = false;
            r.ms = 0;
            r.over = 0;
        } else if (r.kind == "shader") {
            bool replacement = !r.pendingCode.empty();
            auto &code = replacement ? r.pendingCode : r.code;
            if (code.empty())
                return;
            if (r.profile == "vs_3_0") {
                require(caps.VertexShaderVersion >= D3DVS_VERSION(3, 0), "vs_3_0 unsupported");
                IDirect3DVertexShader9 *v = nullptr;
                checked(d->CreateVertexShader(reinterpret_cast<const DWORD *>(code.data()), &v),
                        "CreateVertexShader");
                drop(r.vs);
                r.vs = v;
            } else {
                IDirect3DPixelShader9 *p = nullptr;
                checked(d->CreatePixelShader(reinterpret_cast<const DWORD *>(code.data()), &p),
                        "CreatePixelShader");
                HRESULT hr = panels.install_user(d, int(r.id), code, r.generation);
                if (FAILED(hr)) {
                    drop(p);
                    checked(hr, "Install panel shader");
                }
                drop(r.ps);
                r.ps = p;
            }
            if (replacement) {
                r.code = std::move(r.pendingCode);
                r.constants = r.pendingConstants;
                r.installedGeneration = r.generation;
                r.jobState = "ready";
                r.error.clear();
            }
            r.disabled = false;
            r.ms = 0;
            r.over = 0;
        } else if (r.kind == "cube" || r.kind == "volume") {
            unsigned w = number(r.config, "width", 0, r.kind == "cube" ? 1024 : 256),
                     h = r.kind == "cube" ? w : number(r.config, "height", w, 256),
                     depth = r.kind == "cube" ? 1 : number(r.config, "depth", w, 256),
                     levels = number(r.config, "levels", 1, 11);
            require(w && h && depth && levels, "invalid cube/volume dimensions");
            if (r.kind == "cube")
                h = w;
            unsigned maxDim = std::max(w, std::max(h, depth)), maxLevels = 1;
            while (maxDim > 1) {
                maxDim >>= 1;
                ++maxLevels;
            }
            require(levels <= maxLevels, "too many mip levels");
            size_t expected = 0;
            for (unsigned face = 0; face < (r.kind == "cube" ? 6u : 1u); ++face)
                for (unsigned l = 0; l < levels; ++l)
                    expected += size_t(std::max(1u, w >> l)) * std::max(1u, h >> l) *
                                (r.kind == "cube" ? 1 : std::max(1u, depth >> l)) * 4;
            cpuBudget(expected, r.id);
            if (r.bytes.empty())
                r.bytes.assign(expected, 0);
            require(r.bytes.size() == expected, "cube/volume bytes must include every face and mip");
            size_t offset = 0;
            HRESULT hr = S_OK;
            if (r.kind == "cube") {
                IDirect3DCubeTexture9 *cpu = nullptr, *gpu = nullptr;
                checked(d->CreateCubeTexture(w, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &cpu, nullptr),
                        "Create cube staging");
                for (unsigned face = 0; face < 6 && SUCCEEDED(hr); ++face)
                    for (unsigned l = 0; l < levels && SUCCEEDED(hr); ++l) {
                        unsigned size = std::max(1u, w >> l);
                        D3DLOCKED_RECT lr{};
                        hr = cpu->LockRect(D3DCUBEMAP_FACES(face), l, &lr, nullptr, 0);
                        if (SUCCEEDED(hr)) {
                            for (unsigned y = 0; y < size; ++y)
                                memcpy(static_cast<BYTE *>(lr.pBits) + y * lr.Pitch,
                                       r.bytes.data() + offset + y * size * 4, size * 4);
                            cpu->UnlockRect(D3DCUBEMAP_FACES(face), l);
                        }
                        offset += size_t(size) * size * 4;
                    }
                if (SUCCEEDED(hr))
                    hr = d->CreateCubeTexture(w, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gpu, nullptr);
                if (SUCCEEDED(hr))
                    hr = d->UpdateTexture(cpu, gpu);
                drop(cpu);
                if (FAILED(hr)) {
                    drop(gpu);
                    checked(hr, "Upload cube texture");
                }
                drop(r.cube);
                r.cube = gpu;
            } else {
                IDirect3DVolumeTexture9 *cpu = nullptr, *gpu = nullptr;
                checked(d->CreateVolumeTexture(w, h, depth, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                               &cpu, nullptr),
                        "Create volume staging");
                for (unsigned l = 0; l < levels && SUCCEEDED(hr); ++l) {
                    unsigned x = std::max(1u, w >> l), y = std::max(1u, h >> l), z = std::max(1u, depth >> l);
                    D3DLOCKED_BOX box{};
                    hr = cpu->LockBox(l, &box, nullptr, 0);
                    if (SUCCEEDED(hr)) {
                        for (unsigned k = 0; k < z; ++k)
                            for (unsigned j = 0; j < y; ++j)
                                memcpy(static_cast<BYTE *>(box.pBits) + k * box.SlicePitch + j * box.RowPitch,
                                       r.bytes.data() + offset + (size_t(k) * y + j) * x * 4, x * 4);
                        cpu->UnlockBox(l);
                    }
                    offset += size_t(x) * y * z * 4;
                }
                if (SUCCEEDED(hr))
                    hr = d->CreateVolumeTexture(w, h, depth, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                &gpu, nullptr);
                if (SUCCEEDED(hr))
                    hr = d->UpdateTexture(cpu, gpu);
                drop(cpu);
                if (FAILED(hr)) {
                    drop(gpu);
                    checked(hr, "Upload volume texture");
                }
                drop(r.volume);
                r.volume = gpu;
            }
            r.defined = true;
        } else if (r.kind == "texture" || r.kind == "target") {
            unsigned w = number(r.config, "width", 0, std::min(2048u, unsigned(caps.MaxTextureWidth))),
                     h = number(r.config, "height", 0, std::min(2048u, unsigned(caps.MaxTextureHeight)));
            require(w && h, "invalid texture dimensions");
            D3DFORMAT format = D3DFMT_A8R8G8B8;
            auto f = r.config.value("format", std::string("rgba8"));
            if (f == "rgba16f")
                format = D3DFMT_A16B16G16R16F;
            else if (f == "rgba32f")
                format = D3DFMT_A32B32G32R32F;
            else
                require(f == "rgba8", "unsupported format");
            IDirect3DTexture9 *t = nullptr;
            IDirect3DSurface9 *s = nullptr, *z = nullptr;
            if (r.kind == "target") {
                checked(
                    d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &t, nullptr),
                    "Create target");
                HRESULT hr = t->GetSurfaceLevel(0, &s);
                if (FAILED(hr)) {
                    drop(t);
                    checked(hr, "Get target surface");
                }
                if (r.config.value("depth", false)) {
                    hr = d->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &z,
                                                      nullptr);
                    if (FAILED(hr)) {
                        drop(s);
                        drop(t);
                        checked(hr, "Create depth/stencil");
                    }
                }
            } else {
                unsigned bpp = format == D3DFMT_A8R8G8B8 ? 4 : format == D3DFMT_A16B16G16R16F ? 8 : 16;
                unsigned levels = number(r.config, "levels", 1, 12), maxLevels = 1, m = std::max(w, h);
                while (m > 1) {
                    m >>= 1;
                    ++maxLevels;
                }
                require(levels && levels <= maxLevels, "invalid mip level count");
                size_t size = 0;
                for (unsigned l = 0; l < levels; ++l)
                    size += size_t(std::max(1u, w >> l)) * std::max(1u, h >> l) * bpp;
                cpuBudget(size, r.id);
                if (r.bytes.empty())
                    r.bytes.assign(size, 0);
                require(r.bytes.size() == size, "texture bytes must include every mip level");
                IDirect3DTexture9 *cpu = nullptr;
                checked(d->CreateTexture(w, h, levels, 0, format, D3DPOOL_SYSTEMMEM, &cpu, nullptr),
                        "Create staging texture");
                HRESULT hr = S_OK;
                size_t offset = 0;
                for (unsigned l = 0; l < levels && SUCCEEDED(hr); ++l) {
                    unsigned x = std::max(1u, w >> l), y = std::max(1u, h >> l);
                    D3DLOCKED_RECT lr{};
                    hr = cpu->LockRect(l, &lr, nullptr, 0);
                    if (SUCCEEDED(hr)) {
                        for (unsigned row = 0; row < y; ++row)
                            memcpy(static_cast<BYTE *>(lr.pBits) + row * lr.Pitch,
                                   r.bytes.data() + offset + size_t(row) * x * bpp, x * bpp);
                        cpu->UnlockRect(l);
                    }
                    offset += size_t(x) * y * bpp;
                }
                if (SUCCEEDED(hr))
                    hr = d->CreateTexture(w, h, levels, 0, format, D3DPOOL_DEFAULT, &t, nullptr);
                if (SUCCEEDED(hr))
                    hr = d->UpdateTexture(cpu, t);
                drop(cpu);
                if (FAILED(hr)) {
                    drop(t);
                    checked(hr, "Upload texture");
                }
            }
            drop(r.texture);
            drop(r.surface);
            drop(r.depth);
            r.texture = t;
            r.surface = s;
            r.depth = z;
            r.defined = r.kind == "texture";
        } else if (r.kind == "vertices") {
            IDirect3DVertexBuffer9 *v = reuseBuffer ? r.vb : nullptr;
            if (!v) {
                checked(d->CreateVertexBuffer(UINT(r.bytes.size()),
                                              D3DUSAGE_WRITEONLY | (dynamicBuffer ? D3DUSAGE_DYNAMIC : 0), 0,
                                              D3DPOOL_DEFAULT, &v, nullptr),
                        "CreateVertexBuffer");
                ++bufferCreations;
            }
            void *data = nullptr;
            const auto hr = v->Lock(0, UINT(r.bytes.size()), &data, dynamicBuffer ? D3DLOCK_DISCARD : 0);
            if (FAILED(hr)) {
                if (!reuseBuffer)
                    drop(v);
                checked(hr, "Lock vertex buffer");
            }
            memcpy(data, r.bytes.data(), r.bytes.size());
            const auto unlocked = v->Unlock();
            if (FAILED(unlocked)) {
                if (!reuseBuffer)
                    drop(v);
                checked(unlocked, "Unlock vertex buffer");
            }
            if (!reuseBuffer) {
                drop(r.vb);
                r.vb = v;
            }
            ++bufferUploads;
            bufferUploadedBytes += r.bytes.size();
        } else if (r.kind == "indices") {
            IDirect3DIndexBuffer9 *b = reuseBuffer ? r.ib : nullptr;
            if (!b) {
                checked(d->CreateIndexBuffer(UINT(r.bytes.size()),
                                             D3DUSAGE_WRITEONLY | (dynamicBuffer ? D3DUSAGE_DYNAMIC : 0),
                                             D3DFMT_INDEX32, D3DPOOL_DEFAULT, &b, nullptr),
                        "CreateIndexBuffer");
                ++bufferCreations;
            }
            void *data = nullptr;
            const auto hr = b->Lock(0, UINT(r.bytes.size()), &data, dynamicBuffer ? D3DLOCK_DISCARD : 0);
            if (FAILED(hr)) {
                if (!reuseBuffer)
                    drop(b);
                checked(hr, "Lock index buffer");
            }
            memcpy(data, r.bytes.data(), r.bytes.size());
            const auto unlocked = b->Unlock();
            if (FAILED(unlocked)) {
                if (!reuseBuffer)
                    drop(b);
                checked(unlocked, "Unlock index buffer");
            }
            if (!reuseBuffer) {
                drop(r.ib);
                r.ib = b;
            }
            ++bufferUploads;
            bufferUploadedBytes += r.bytes.size();
        } else if (r.kind == "layout") {
            std::vector<D3DVERTEXELEMENT9> elements;
            auto fields = r.config.at("elements");
            require(fields.is_array() && fields.size() && fields.size() <= 32, "invalid vertex layout");
            static const std::map<std::string, BYTE> types = {
                {"float1", 0},   {"float2", 1},    {"float3", 2},    {"float4", 3},  {"color", 4},
                {"ubyte4", 5},   {"short2", 6},    {"short4", 7},    {"ubyte4n", 8}, {"short2n", 9},
                {"short4n", 10}, {"ushort2n", 11}, {"ushort4n", 12}, {"udec3", 13},  {"dec3n", 14},
                {"half2", 15},   {"half4", 16}};
            static const std::map<std::string, BYTE> usages = {
                {"position", 0}, {"blendweight", 1}, {"blendindices", 2}, {"normal", 3},     {"psize", 4},
                {"texcoord", 5}, {"tangent", 6},     {"binormal", 7},     {"tessfactor", 8}, {"positiont", 9},
                {"color", 10},   {"fog", 11},        {"depth", 12},       {"sample", 13}};
            std::set<std::pair<unsigned, unsigned>> semantics;
            for (auto &e : fields) {
                auto t = types.find(e.at("type").get<std::string>()),
                     u = usages.find(e.value("usage", std::string("position")));
                require(t != types.end() && u != usages.end(), "unknown vertex type/usage");
                unsigned stream = number(e, "stream", 0, 15), offset = number(e, "offset", 0, 2047),
                         index = number(e, "index", 0, 15);
                require(semantics.insert({u->second, index}).second, "duplicate vertex semantic");
                elements.push_back(
                    {WORD(stream), WORD(offset), t->second, D3DDECLMETHOD_DEFAULT, u->second, BYTE(index)});
            }
            D3DVERTEXELEMENT9 end = D3DDECL_END();
            elements.push_back(end);
            IDirect3DVertexDeclaration9 *v = nullptr;
            checked(d->CreateVertexDeclaration(elements.data(), &v), "CreateVertexDeclaration");
            drop(r.decl);
            r.decl = v;
            r.elements = std::move(elements);
        }
        r.gpuBytes = std::max(cost, r.bytes.size());
        r.state = "ready";
        if (!r.program())
            r.error.clear();
        r.dirty = false;
    }
    // Asynchronous GPU timing and optional profiling. Never flush or spin for results.
    void profileDrop(TimingFrame &t) {
        drop(t.wholeStart);
        drop(t.wholeEnd);
        drop(t.copyEnd);
        t.profiled = false;
        t.copyMeasured = false;
    }
    HRESULT queryData(IDirect3DQuery9 *query, void *data, DWORD size,
                      [[maybe_unused]] bool optional = false) {
#ifdef HLSL_STANDALONE_TEST
        if (testQueryFailure == (optional ? 3u : 2u)) {
            testQueryFailure = 0;
            return E_FAIL;
        }
#endif
        return query->GetData(data, size, 0);
    }
    void queryDrop(TimingFrame &t) {
        for (auto &q : t.draws) {
            drop(q.start);
            drop(q.end);
        }
        drop(t.frequency);
        drop(t.disjoint);
        profileDrop(t);
    }
    bool queryCreate(IDirect3DDevice9 *d, D3DQUERYTYPE type, IDirect3DQuery9 *&query) {
#ifdef HLSL_STANDALONE_TEST
        if (testQueryFailure == 1 && type == D3DQUERYTYPE_TIMESTAMP) {
            testQueryFailure = 0;
            ++queryFailures;
            return false;
        }
#endif
        if (query) {
            ++queriesReused;
            return true;
        }
        if (FAILED(d->CreateQuery(type, &query))) {
            ++queryFailures;
            return false;
        }
        ++queriesCreated;
        return true;
    }
    void timingPoll() {
        profile9::Scope measured(profile9::Timing);
        for (auto it = timings.begin(); it != timings.end();) {
            UINT64 freq = 0;
            BOOL disjoint = FALSE;
            const auto fh = queryData(it->frequency, &freq, sizeof(freq));
            const auto dh = queryData(it->disjoint, &disjoint, sizeof(disjoint));
            bool failed = FAILED(fh) || FAILED(dh), ready = true;
            if (!failed && (fh == S_FALSE || dh == S_FALSE)) {
                ++it;
                continue;
            }
            std::map<unsigned, double> sums;
            for (size_t i = 0; !failed && i < it->used; ++i) {
                auto &q = it->draws[i];
                UINT64 a = 0, b = 0;
                const auto x = queryData(q.start, &a, sizeof(a));
                const auto y = queryData(q.end, &b, sizeof(b));
                if (FAILED(x) || FAILED(y)) {
                    failed = true;
                    break;
                }
                if (x == S_FALSE || y == S_FALSE) {
                    ready = false;
                    break;
                }
                if (freq && b >= a) {
                    const double ms = double(b - a) * 1000.0 / double(freq);
                    sums[q.id] += ms;
                    if (q.vertex)
                        sums[q.vertex] += ms;
                }
            }
            UINT64 wholeStart = 0, wholeEnd = 0, copyEnd = 0;
            bool profileReady = it->profiled;
            if (!failed && ready && it->profiled) {
                const auto a = queryData(it->wholeStart, &wholeStart, sizeof(wholeStart), true);
                const auto b = queryData(it->wholeEnd, &wholeEnd, sizeof(wholeEnd), true);
                const auto c =
                    it->copyMeasured ? queryData(it->copyEnd, &copyEnd, sizeof(copyEnd), true) : S_OK;
                if (FAILED(a) || FAILED(b) || FAILED(c)) {
                    profileReady = false;
                    ++queryFailures;
                    profileDrop(*it);
                } else if (a == S_FALSE || b == S_FALSE || c == S_FALSE)
                    ready = false;
            }
            if (!failed && !ready) {
                ++it;
                continue;
            }
            if (!failed && !disjoint && freq && profileReady &&
                it->profileEpoch == profile9::epoch.load(std::memory_order_relaxed) &&
                wholeEnd >= wholeStart) {
                const double ms = double(wholeEnd - wholeStart) * 1000.0 / double(freq);
                ++gpuSamples;
                gpuTotalMs += ms;
                gpuMaxMs = std::max(gpuMaxMs, ms);
                if (it->copyMeasured && copyEnd >= wholeStart) {
                    const double copy = double(copyEnd - wholeStart) * 1000.0 / double(freq);
                    ++copySamples;
                    copyTotalMs += copy;
                    copyMaxMs = std::max(copyMaxMs, copy);
                }
            }
            if (!failed && !disjoint && freq)
                for (auto &sum : sums) {
                    auto r = resources.find(sum.first);
                    if (r == resources.end())
                        continue;
                    auto &v = *r->second;
                    v.ms = v.ms ? 0.97 * v.ms + 0.03 * sum.second : sum.second;
                    const double budget = owners.at(v.owner).budget;
                    if (v.ms > budget) {
                        if (++v.over >= 90) {
                            v.disabled = true;
                            v.error = "GPU budget exceeded for 90 measured frames";
                        }
                    } else
                        v.over = 0;
                }
            if (failed) {
                ++queryFailures;
                queryDrop(*it);
            } else
                timingPool.push_back(std::move(*it));
            it = timings.erase(it);
        }
    }
    void timingBegin(IDirect3DDevice9 *d) {
        profile9::Scope measured(profile9::Timing);
        // render() has already polled. Eight frames bound all active/free sets;
        // a delayed GPU causes sampling to defer, never a synchronous flush.
        if (timings.size() >= 8)
            return;
        if (!timingPool.empty()) {
            activeTiming = std::move(timingPool.back());
            timingPool.pop_back();
        }
        activeTiming.used = 0;
        activeTiming.valid = true;
        activeTiming.profiled = false;
        activeTiming.copyMeasured = false;
        activeTiming.profileEpoch = profile9::epoch.load(std::memory_order_relaxed);
        openTiming = SIZE_MAX;
        if (!queryCreate(d, D3DQUERYTYPE_TIMESTAMPFREQ, activeTiming.frequency) ||
            !queryCreate(d, D3DQUERYTYPE_TIMESTAMPDISJOINT, activeTiming.disjoint)) {
            queryDrop(activeTiming);
            activeTiming = {};
            return;
        }
        if (FAILED(activeTiming.disjoint->Issue(D3DISSUE_BEGIN))) {
            ++queryFailures;
            queryDrop(activeTiming);
            activeTiming = {};
            return;
        }
        timingSupported = true;
        if (profile9::enabled.load(std::memory_order_relaxed) &&
            queryCreate(d, D3DQUERYTYPE_TIMESTAMP, activeTiming.wholeStart) &&
            queryCreate(d, D3DQUERYTYPE_TIMESTAMP, activeTiming.wholeEnd) &&
            queryCreate(d, D3DQUERYTYPE_TIMESTAMP, activeTiming.copyEnd)) {
            if (SUCCEEDED(activeTiming.wholeStart->Issue(D3DISSUE_END)))
                activeTiming.profiled = true;
            else {
                ++queryFailures;
                profileDrop(activeTiming);
            }
        }
    }
    void timingDraw(IDirect3DDevice9 *d, unsigned id, bool begin, unsigned vertex = 0) {
        profile9::Scope measured(profile9::Timing);
        if (!activeTiming.frequency || !activeTiming.valid || !id)
            return;
        if (begin) {
            openTiming = SIZE_MAX;
            if (activeTiming.used == activeTiming.draws.size())
                activeTiming.draws.emplace_back();
            auto &q = activeTiming.draws[activeTiming.used];
            q.id = id;
            q.vertex = vertex;
            if (!queryCreate(d, D3DQUERYTYPE_TIMESTAMP, q.start) ||
                !queryCreate(d, D3DQUERYTYPE_TIMESTAMP, q.end) || FAILED(q.start->Issue(D3DISSUE_END))) {
                activeTiming.valid = false;
                ++queryFailures;
                return;
            }
            openTiming = activeTiming.used++;
        } else if (openTiming != SIZE_MAX) {
            auto &q = activeTiming.draws[openTiming];
            if (q.id != id || FAILED(q.end->Issue(D3DISSUE_END))) {
                activeTiming.valid = false;
                ++queryFailures;
            }
            openTiming = SIZE_MAX;
        }
    }
    void timingEnd() {
        if (!activeTiming.frequency)
            return;
        if (activeTiming.profiled && FAILED(activeTiming.wholeEnd->Issue(D3DISSUE_END))) {
            profileDrop(activeTiming);
            ++queryFailures;
        }
        const auto dh = activeTiming.disjoint->Issue(D3DISSUE_END);
        const auto fh = activeTiming.frequency->Issue(D3DISSUE_END);
        if (!activeTiming.valid || openTiming != SIZE_MAX || FAILED(dh) || FAILED(fh)) {
            ++queryFailures;
            queryDrop(activeTiming);
        } else
            timings.push_back(std::move(activeTiming));
        activeTiming = {};
        openTiming = SIZE_MAX;
    }
    // Capture and pass execution. Every pass restores the host device state.
    void captureFrame(IDirect3DDevice9 *d) {
        profile9::Scope measured(profile9::Capture);
        IDirect3DSurface9 *back = nullptr;
        checked(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back), "GetBackBuffer");
        D3DSURFACE_DESC desc{};
        HRESULT hr = back->GetDesc(&desc);
        if (SUCCEEDED(hr) && (!capture || captureW != desc.Width || captureH != desc.Height)) {
            drop(captureSurface);
            drop(capture);
            hr = d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                  D3DPOOL_DEFAULT, &capture, nullptr);
            if (SUCCEEDED(hr))
                hr = capture->GetSurfaceLevel(0, &captureSurface);
            captureW = desc.Width;
            captureH = desc.Height;
        }
        if (SUCCEEDED(hr))
            hr = d->StretchRect(back, nullptr, captureSurface, nullptr, D3DTEXF_NONE);
        drop(back);
        checked(hr, "Capture/resolve backbuffer");
        ++screenCaptures;
    }
    Resource &ready(unsigned owner, unsigned id, const char *kind = nullptr) {
        auto &r = get(owner, id, kind);
        require(r.state == "ready" || (r.program() && (r.ps || r.vs || r.effect)),
                "resource is not GPU-ready");
        return r;
    }
    void constants(IDirect3DDevice9 *d, bool vs, const J &c, bool skipFloat = false) {
        if (c.empty())
            return;
        for (auto type : {"float", "int", "bool"}) {
            if (skipFloat && type[0] == 'f')
                continue;
            const auto &values = field(c, type, emptyArray);
            require(values.is_array(), "constants must be an array of register blocks");
            for (auto &b : values) {
                unsigned max = std::string(type) == "float" ? (vs ? caps.MaxVertexShaderConst : 224) : 16;
                unsigned start = number(b, "register", 0, max - 1);
                const auto &arr = b.at("values");
                require(arr.is_array(), "constant values must be arrays");
                if (std::string(type) == "float") {
                    auto v = floats(arr, max * 4);
                    require(v.size() % 4 == 0 && start + v.size() / 4 <= max, "float constant range");
                    if (!v.empty())
                        checked(vs ? d->SetVertexShaderConstantF(start, v.data(), UINT(v.size() / 4))
                                   : d->SetPixelShaderConstantF(start, v.data(), UINT(v.size() / 4)),
                                "Set float constants");
                } else if (std::string(type) == "int") {
                    require(arr.size() % 4 == 0 && start + arr.size() / 4 <= 16, "int constant range");
                    std::vector<int> v;
                    v.reserve(arr.size());
                    for (auto &x : arr) {
                        require(x.is_number_integer(), "integer constant required");
                        auto n = x.get<int64_t>();
                        require(n >= INT_MIN && n <= INT_MAX, "int32 range");
                        v.push_back(int(n));
                    }
                    if (!v.empty())
                        checked(vs ? d->SetVertexShaderConstantI(start, v.data(), UINT(v.size() / 4))
                                   : d->SetPixelShaderConstantI(start, v.data(), UINT(v.size() / 4)),
                                "Set integer constants");
                } else {
                    require(start + arr.size() <= 16, "bool constant range");
                    std::vector<BOOL> v;
                    v.reserve(arr.size());
                    for (auto &x : arr) {
                        require(x.is_boolean(), "boolean constant required");
                        v.push_back(x.get<bool>());
                    }
                    if (!v.empty())
                        checked(vs ? d->SetVertexShaderConstantB(start, v.data(), UINT(v.size()))
                                   : d->SetPixelShaderConstantB(start, v.data(), UINT(v.size())),
                                "Set boolean constants");
                }
            }
        }
    }
    void pass(IDirect3DDevice9 *d, unsigned owner, const J &p, const PreparedPass *prepared = nullptr,
              const std::vector<float> *uniforms = nullptr) {
        profile9::Scope measured(profile9::Pass);
        unsigned effectID = number(p, "effect", 0, 16777215), firstFXPass = 0, fxPassCount = 1;
        if (effectID) {
            auto &r = ready(owner, effectID, "effect");
            require(r.effect && !r.disabled, "effect unavailable or disabled");
            require(!p.contains("pixel_shader") && !p.contains("vertex_shader"),
                    "FX supplies its own shaders");
            auto count = r.effect->passes(p.value("technique", std::string()));
            fxPassCount = count;
            if (p.contains("effect_pass")) {
                firstFXPass = number(p, "effect_pass", 0, 127);
                require(firstFXPass < count, "FX pass index outside technique");
                fxPassCount = 1;
            }
        }
        State scope(d, stateCache, caps);
        std::vector<unsigned> targets;
        IDirect3DSurface9 *first = nullptr;
        D3DSURFACE_DESC desc{};
        const auto &output = field(p, "targets", emptyArray);
        require(output.is_array() && output.size() <= std::min(4u, unsigned(caps.NumSimultaneousRTs)),
                "too many render targets");
        if (output.empty()) {
            checked(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &first), "Get backbuffer");
            checked(first->GetDesc(&desc), "Backbuffer description");
        } else {
            auto &t = ready(owner, output[0].get<unsigned>(), "target");
            first = t.surface;
            first->AddRef();
            checked(first->GetDesc(&desc), "Target description");
        }
        d->SetDepthStencilSurface(nullptr);
        HRESULT hr = d->SetRenderTarget(0, first);
        drop(first);
        checked(hr, "Set target");
        for (unsigned i = 1; i < scope.n; ++i)
            checked(d->SetRenderTarget(i, nullptr), "Clear additional target");
        for (unsigned i = 0; i < output.size(); ++i) {
            unsigned id = output[i].get<unsigned>();
            auto &t = ready(owner, id, "target");
            D3DSURFACE_DESC td{};
            checked(t.surface->GetDesc(&td), "Target description");
            require(td.Width == desc.Width && td.Height == desc.Height, "MRT size mismatch");
            require(std::find(targets.begin(), targets.end(), id) == targets.end(), "duplicate MRT target");
            targets.push_back(id);
            checked(d->SetRenderTarget(i, t.surface), "Set MRT");
            if (i == 0)
                checked(d->SetDepthStencilSurface(t.depth), "Set pass depth");
        }
        D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0, 1};
        const auto &v = field(p, "viewport", emptyArray);
        if (!v.empty()) {
            require(v.is_array() && v.size() == 6, "viewport is x,y,w,h,minz,maxz");
            vp = {v[0].get<DWORD>(), v[1].get<DWORD>(), v[2].get<DWORD>(),
                  v[3].get<DWORD>(), scalar(v[4]),      scalar(v[5])};
            require(vp.Width && vp.Height && uint64_t(vp.X) + vp.Width <= desc.Width &&
                        uint64_t(vp.Y) + vp.Height <= desc.Height && vp.MinZ >= 0 && vp.MaxZ <= 1 &&
                        vp.MinZ <= vp.MaxZ,
                    "viewport outside target");
        }
        checked(d->SetViewport(&vp), "Set viewport");
        // Establish a deterministic baseline; restore every touched state at scope exit.
        for (auto kv : renderBaseline)
            checked(d->SetRenderState(kv.first, kv.second), "Set baseline state");
        for (auto kv : geometryBaseline)
            checked(d->SetRenderState(kv.first, kv.second), "Set geometry baseline");
        for (unsigned i = 1; i < scope.n; ++i)
            checked(d->SetRenderState(D3DRENDERSTATETYPE(D3DRS_COLORWRITEENABLE1 + i - 1), 15),
                    "Set MRT color mask");
        for (unsigned i = 0; i < 8; ++i) {
            checked(d->SetTextureStageState(i, D3DTSS_TEXCOORDINDEX, i), "Fixed-function UV index");
            checked(d->SetTextureStageState(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE),
                    "Fixed-function UV transform");
        }
        auto blend = p.value("blend", std::string("opaque"));
        if (blend != "opaque") {
            require(blend == "alpha" || blend == "premultiplied" || blend == "add", "invalid blend");
            checked(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE), "Blend enable");
            checked(d->SetRenderState(D3DRS_SRCBLEND,
                                      blend == "premultiplied" ? D3DBLEND_ONE : D3DBLEND_SRCALPHA),
                    "Blend source");
            checked(d->SetRenderState(D3DRS_DESTBLEND, blend == "add" ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA),
                    "Blend destination");
            d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
            d->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
            d->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
            d->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
        }
        for (auto &s : field(p, "states", emptyArray)) {
            unsigned k = number(s, "state", 0, 209);
            require(k >= 7, "invalid render state");
            checked(d->SetRenderState(D3DRENDERSTATETYPE(k), number(s, "value", 0, 0xffffffff)),
                    "Set render state");
        }
        const auto &clip = field(p, "scissor", emptyArray);
        if (!clip.empty()) {
            require(clip.is_array() && clip.size() == 4, "scissor is left,top,right,bottom");
            RECT r = {clip[0].get<LONG>(), clip[1].get<LONG>(), clip[2].get<LONG>(), clip[3].get<LONG>()};
            require(r.left >= 0 && r.top >= 0 && r.right >= r.left && r.bottom >= r.top &&
                        r.right <= LONG(desc.Width) && r.bottom <= LONG(desc.Height),
                    "invalid scissor");
            checked(d->SetScissorRect(&r), "Set scissor");
            checked(d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE), "Enable scissor");
        }
        const auto &clear = field(p, "clear", emptyObject);
        for (auto id : targets)
            require(get(owner, id).defined || clear.contains("color"),
                    "first pass after creation/reset must clear a render target");
        if (!clear.empty()) {
            DWORD flags = 0, color = 0;
            float z = 1;
            DWORD stencil = 0;
            if (clear.contains("color")) {
                auto c = floats(clear["color"], 4);
                require(c.size() == 4, "clear color is RGBA");
                for (auto x : c)
                    require(x >= 0 && x <= 1, "clear color range");
                color = D3DCOLOR_COLORVALUE(c[0], c[1], c[2], c[3]);
                flags |= D3DCLEAR_TARGET;
            }
            if (clear.contains("depth")) {
                z = scalar(clear["depth"]);
                require(z >= 0 && z <= 1, "clear depth range");
                flags |= D3DCLEAR_ZBUFFER;
            }
            if (clear.contains("stencil")) {
                stencil = number(clear, "stencil", 0, 255);
                flags |= D3DCLEAR_STENCIL;
            }
            checked(d->Clear(0, nullptr, flags, color, z, stencil), "Clear pass");
            ++clearCalls;
        }
        if (p.value("clear_only", false)) {
            require(!clear.empty(), "clear_only requires a clear description");
            for (auto id : targets)
                get(owner, id).defined = true;
            return;
        }
        unsigned psid = effectID ? effectID : number(p, "pixel_shader", 0, 16777215),
                 vsid = number(p, "vertex_shader", 0, 16777215);
        bool hasVertexShader = vsid != 0;
        struct EndEffect {
            fx9::Effect *effect = nullptr;
            ~EndEffect() {
                if (effect)
                    effect->end();
            }
        } endEffect;
        for (unsigned slot = 0; slot < 16; ++slot)
            d->SetTexture(slot, nullptr);
        for (unsigned slot = 0; slot < 4; ++slot)
            d->SetTexture(D3DVERTEXTEXTURESAMPLER0 + slot, nullptr);
        if (effectID) {
            auto &r = ready(owner, effectID, "effect");
            require(r.effect && !r.disabled, "effect unavailable or disabled");
            d->SetPixelShader(nullptr);
            d->SetVertexShader(nullptr);
            J values = r.config.value("parameters", J::object());
            auto overrides = p.value("parameters", J::object());
            require((values.is_object() || values.empty()) && (overrides.is_object() || overrides.empty()),
                    "FX parameters must be objects");
            if (values.empty())
                values = J::object();
            if (!overrides.empty())
                values.merge_patch(overrides);
            require(values.size() <= 256 && values.dump().size() <= 262144,
                    "FX parameter block exceeds limits");
            r.config["parameters"] = values;
            endEffect.effect = r.effect.get();
            r.effect->start(p.value("technique", std::string()), firstFXPass, values,
                            [&](const J &value) -> IDirect3DBaseTexture9 * {
                                if (value.value("capture", false))
                                    return capture;
                                unsigned id = number(value, "texture", 0, 16777215);
                                if (!id)
                                    return nullptr;
                                require(std::find(targets.begin(), targets.end(), id) == targets.end(),
                                        "FX cannot sample an active render target");
                                auto &tex = ready(owner, id);
                                require(tex.base() && tex.defined, "FX texture is not ready/defined");
                                return tex.base();
                            });
            IDirect3DPixelShader9 *boundPS = nullptr;
            checked(d->GetPixelShader(&boundPS), "FX GetPixelShader");
            drop(boundPS);
            IDirect3DVertexShader9 *boundVS = nullptr;
            checked(d->GetVertexShader(&boundVS), "FX GetVertexShader");
            hasVertexShader = boundVS != nullptr;
            drop(boundVS);
        } else {
            auto &ps = ready(owner, psid, "shader");
            require(ps.ps && !ps.disabled, "pixel shader unavailable or disabled");
            checked(d->SetPixelShader(ps.ps), "Set pixel shader");
            if (vsid) {
                auto &vs = ready(owner, vsid, "shader");
                require(vs.vs && !vs.disabled, "vertex shader unavailable");
                checked(d->SetVertexShader(vs.vs), "Set vertex shader");
            } else
                checked(d->SetVertexShader(nullptr), "Clear vertex shader");
            float zeros[256 * 4]{};
            int izero[64]{};
            BOOL bzero[16]{};
            d->SetPixelShaderConstantF(0, zeros, 224);
            d->SetPixelShaderConstantI(0, izero, 16);
            d->SetPixelShaderConstantB(0, bzero, 16);
            if (hasVertexShader) {
                d->SetVertexShaderConstantF(0, zeros, caps.MaxVertexShaderConst);
                d->SetVertexShaderConstantI(0, izero, 16);
                d->SetVertexShaderConstantB(0, bzero, 16);
            }
        }
        auto applyBindings = [&]() {
            profile9::Scope measuredBindings(profile9::Bindings);
            if (prepared)
                for (const auto &block : prepared->uniforms) {
                    require(block.reg < (block.vertex ? caps.MaxVertexShaderConst : 224) &&
                                block.reg + block.count <= (block.vertex ? caps.MaxVertexShaderConst : 224),
                            "float constant range");
                    if (block.count)
                        checked(block.vertex ? d->SetVertexShaderConstantF(
                                                   block.reg, uniforms->data() + block.offset, block.count)
                                             : d->SetPixelShaderConstantF(
                                                   block.reg, uniforms->data() + block.offset, block.count),
                                "Set prepared constants");
                }
            constants(d, false, field(p, "ps_constants", emptyObject), prepared != nullptr);
            constants(d, true, field(p, "vs_constants", emptyObject), prepared != nullptr);
            const auto &bind = field(p, "textures", emptyArray);
            require(bind.is_array() && bind.size() <= 20, "texture binding limit");
            for (auto &b : bind) {
                auto stage = b.value("stage", std::string("ps"));
                require(stage == "ps" || stage == "vs", "sampler stage must be ps or vs");
                bool vertex = stage == "vs";
                unsigned slot = number(b, "slot", 0, vertex ? 3 : 15),
                         native = vertex ? D3DVERTEXTEXTURESAMPLER0 + slot : slot;
                IDirect3DBaseTexture9 *t = nullptr;
                if (b.contains("capture") && b["capture"].get<bool>())
                    t = capture;
                else {
                    unsigned id = number(b, "texture", 0, 16777215);
                    require(std::find(targets.begin(), targets.end(), id) == targets.end(),
                            "cannot sample an active render target; use ping-pong targets");
                    auto &r = ready(owner, id);
                    require(r.kind == "texture" || r.kind == "target" || r.kind == "cube" ||
                                r.kind == "volume",
                            "expected texture/target");
                    require(r.defined,
                            "render target contents were lost/reset; clear or redraw before sampling");
                    t = r.base();
                }
                checked(d->SetTexture(native, t), "Bind texture");
                for (auto kv : samplerBaseline)
                    checked(d->SetSamplerState(native, kv.first, kv.second), "Sampler baseline");
                for (auto &s : field(b, "sampler", emptyArray)) {
                    unsigned state = number(s, "state", 0, 13);
                    require(state >= 1, "invalid sampler state");
                    checked(d->SetSamplerState(native, D3DSAMPLERSTATETYPE(state),
                                               number(s, "value", 0, 0xffffffff)),
                            "Set sampler state");
                }
            }
        };
        applyBindings();
        checked(d->BeginScene(), "Begin pass");
        bool open = true;
        try {
            timingDraw(d, psid, true, vsid);
            for (unsigned iteration = 0; iteration < fxPassCount; ++iteration) {
                if (iteration) {
                    endEffect.effect->next(firstFXPass + iteration);
                    IDirect3DVertexShader9 *bound = nullptr;
                    checked(d->GetVertexShader(&bound), "FX vertex shader");
                    hasVertexShader = bound != nullptr;
                    drop(bound);
                    applyBindings();
                    timingDraw(d, psid, true, vsid);
                }
                if (p.contains("vertices")) {
                    auto &layout = ready(owner, number(p, "layout", 0, 16777215), "layout");
                    checked(d->SetVertexDeclaration(layout.decl), "Set vertex declaration");
                    const auto &streams = p.at("vertices");
                    require(streams.is_array() && !streams.empty() &&
                                streams.size() <= std::min(16u, unsigned(caps.MaxStreams)),
                            "invalid vertex streams");
                    unsigned vertexCount = UINT_MAX;
                    unsigned instances = number(p, "instances", 1, 1024);
                    require(instances >= 1, "instances must be positive");
                    if (instances > 1)
                        require(hasVertexShader && p.contains("indices"),
                                "instancing requires a vertex shader and indexed geometry");
                    std::set<unsigned> supplied;
                    std::map<unsigned, unsigned> strides;
                    for (auto &stream : streams) {
                        auto &vb = ready(owner, number(stream, "buffer", 0, 16777215), "vertices");
                        unsigned slot = number(stream, "slot", 0, 15),
                                 stride = number(vb.config, "stride", 0, caps.MaxStreamStride),
                                 divisor = number(stream, "divisor", 0, 1024);
                        require(stride && vb.bytes.size() % stride == 0, "invalid vertex stride");
                        require(supplied.insert(slot).second, "duplicate stream");
                        strides[slot] = stride;
                        UINT frequency = 1;
                        if (divisor) {
                            require(slot > 0 && instances > 1, "instance streams must follow stream zero");
                            require(vb.bytes.size() / stride >= (instances + divisor - 1) / divisor,
                                    "instance stream too short");
                            frequency = D3DSTREAMSOURCE_INSTANCEDATA | divisor;
                        } else {
                            vertexCount = std::min(vertexCount, unsigned(vb.bytes.size() / stride));
                            if (instances > 1 && slot == 0)
                                frequency = D3DSTREAMSOURCE_INDEXEDDATA | instances;
                        }
                        checked(d->SetStreamSource(slot, vb.vb, 0, stride), "Bind vertex stream");
                        checked(d->SetStreamSourceFreq(slot, frequency), "Set stream frequency");
                    }
                    require(vertexCount != UINT_MAX && (instances == 1 || supplied.count(0)),
                            "missing per-vertex stream zero");
                    static const unsigned sizes[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
                    for (const auto &e : layout.elements) {
                        if (e.Stream == 0xff)
                            break;
                        require(supplied.count(e.Stream) && e.Type < 17 &&
                                    unsigned(e.Offset) + sizes[e.Type] <= strides[e.Stream],
                                "vertex layout exceeds supplied stream stride");
                    }
                    unsigned primitive = number(p, "primitive", 4, 6), count = number(p, "count", 0, 65536),
                             start = number(p, "start", 0, 1048576);
                    require(primitive >= 1 && count, "invalid primitive/count");
                    unsigned needed = primitive == 1   ? count
                                      : primitive == 2 ? count * 2
                                      : primitive == 3 ? count + 1
                                      : primitive == 4 ? count * 3
                                                       : count + 2;
                    if (p.contains("indices")) {
                        auto &ib = ready(owner, number(p, "indices", 0, 16777215), "indices");
                        require(uint64_t(start) + needed <= ib.bytes.size() / 4,
                                "index range outside buffer");
                        auto ix = reinterpret_cast<const unsigned *>(ib.bytes.data());
                        for (unsigned i = 0; i < needed; ++i)
                            require(ix[start + i] < vertexCount, "index references a missing vertex");
                        checked(d->SetIndices(ib.ib), "Set indices");
                        checked(d->DrawIndexedPrimitive(D3DPRIMITIVETYPE(primitive), 0, 0, vertexCount, start,
                                                        count),
                                "Draw indexed geometry");
                    } else {
                        require(uint64_t(start) + needed <= vertexCount, "draw range outside vertex buffer");
                        checked(d->DrawPrimitive(D3DPRIMITIVETYPE(primitive), start, count), "Draw geometry");
                    }
                } else {
                    struct Vertex {
                        float x, y, z, w, u, v;
                    };
                    float x = float(vp.X) - .5f, y = float(vp.Y) - .5f, w = float(vp.Width),
                          h = float(vp.Height);
                    Vertex q[4] = {{x, y, 0, 1, 0, 0},
                                   {x + w, y, 0, 1, 1, 0},
                                   {x, y + h, 0, 1, 0, 1},
                                   {x + w, y + h, 0, 1, 1, 1}};
                    checked(d->SetStreamSourceFreq(0, 1), "Quad stream frequency");
                    if (hasVertexShader) {
                        q[0].x = q[2].x = -1;
                        q[1].x = q[3].x = 1;
                        q[0].y = q[1].y = 1;
                        q[2].y = q[3].y = -1;
                        D3DVERTEXELEMENT9 e[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0},
                                                 {0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
                                                 D3DDECL_END()};
                        if (!quadDeclaration)
                            checked(d->CreateVertexDeclaration(e, &quadDeclaration), "Quad declaration");
                        checked(d->SetVertexDeclaration(quadDeclaration), "Set quad declaration");
                    } else
                        checked(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1), "Set quad FVF");
                    checked(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex)),
                            "Draw fullscreen quad");
                }
                ++genericDrawCalls;
                timingDraw(d, psid, false);
            }
            if (endEffect.effect) {
                endEffect.effect->end();
                endEffect.effect = nullptr;
            }
            HRESULT hr = d->EndScene();
            open = false;
            checked(hr, "End pass");
            for (auto id : targets)
                get(owner, id).defined = true;
        } catch (...) {
            timingDraw(d, psid, false);
            if (open)
                d->EndScene();
            throw;
        }
    }
    void drawPanels(IDirect3DDevice9 *d, unsigned owner, const J &list) {
        require(list.is_array() && list.size() <= 64, "panel limit 64");
        std::vector<HlslPanel> batch;
        for (auto &p : list) {
            try {
                HlslPanel v{};
                auto rect = floats(p.at("rect"), 4);
                require(rect.size() == 4, "panel rectangle");
                v.x = rect[0];
                v.y = rect[1];
                v.width = rect[2];
                v.height = rect[3];
                auto style = p.value("style", J::object());
                v.radius = scalar(style.value("radius", J(28)));
                v.blur = scalar(style.value("blur", J(3)));
                v.refraction = scalar(style.value("refraction", J(12)));
                v.opacity = scalar(style.value("opacity", J(.94)));
                auto tint = floats(style.value("tint", J::array({.05, .10, .16, .13})), 4);
                require(tint.size() == 4, "panel tint needs RGBA");
                memcpy(v.tint, tint.data(), 16);
                v.edge = scalar(style.value("edge", J(.7)));
                v.dispersion = scalar(style.value("dispersion", J(1.5)));
                v.noise = scalar(style.value("noise", J(.006)));
                v.saturation = scalar(style.value("saturation", J(1.08)));
                v.brightness = scalar(style.value("brightness", J(1.02)));
                auto mode = style.value("material", std::string("liquid"));
                require(mode == "liquid" || mode == "frosted" || mode == "chromatic", "unknown material");
                v.mode = mode == "liquid" ? 0 : mode == "frosted" ? 1 : 2;
                auto blend = style.value("blend", std::string("alpha"));
                require(blend == "alpha" || blend == "add", "panel blend is alpha or add");
                v.blend = blend == "add";
                v.shader = int(number(style, "shader", 0, 16777215));
                if (v.shader) {
                    auto &r = ready(owner, unsigned(v.shader), "shader");
                    require(r.ps && !r.disabled, "panel shader unavailable or disabled");
                    for (unsigned s = 0; s < 3; ++s) {
                        IDirect3DTexture9 *t = nullptr;
                        if (r.panelTextures[s]) {
                            auto &tex = ready(owner, r.panelTextures[s]);
                            require(tex.texture && tex.defined, "panel texture is not ready");
                            t = tex.texture;
                        }
                        panels.bind_texture(v.shader, int(s), t);
                    }
                }
                auto params = style.value("params", J::array());
                require(params.is_array() && params.size() <= 8, "at most eight float4 params");
                for (unsigned i = 0; i < params.size(); ++i) {
                    auto f = floats(params[i], 4);
                    std::copy(f.begin(), f.end(), v.params + i * 4);
                }
                require(HlslRenderer::valid(v), "invalid panel style");
                batch.push_back(v);
            } catch (const std::exception &e) {
                owners[owner].error = std::string("panel skipped: ") + e.what();
            }
        }
        if (batch.empty())
            return;
        panels.timing = [&](int h, bool begin) { timingDraw(d, unsigned(h), begin); };
        HRESULT hr = panels.render(d, batch.data(), UINT(batch.size()),
                                   float(GetTickCount64() % 86400000) / 1000, captureSurface, capture);
        panels.timing = {};
        checked(hr, "Draw panels");
    }
    void readback(IDirect3DDevice9 *d, Resource &r) {
        IDirect3DSurface9 *src = nullptr;
        unsigned id = number(r.config, "target", 0, 16777215);
        if (id) {
            auto &t = ready(r.owner, id, "target");
            require(t.defined, "target must be drawn before readback");
            src = t.surface;
            src->AddRef();
        } else {
            captureFrame(d);
            src = captureSurface;
            src->AddRef();
        }
        D3DSURFACE_DESC desc{};
        HRESULT hr = src->GetDesc(&desc);
        if (FAILED(hr)) {
            drop(src);
            checked(hr, "Readback description");
        }
        if ((desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) ||
            uint64_t(desc.Width) * desc.Height > 1048576) {
            drop(src);
            throw std::runtime_error("readback requires rgba8 and at most 1048576 pixels");
        }
        IDirect3DSurface9 *cpu = nullptr;
        hr = d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &cpu,
                                            nullptr);
        if (SUCCEEDED(hr))
            hr = d->GetRenderTargetData(src, cpu);
        drop(src);
        if (FAILED(hr)) {
            drop(cpu);
            checked(hr, "GPU readback");
        }
        D3DLOCKED_RECT lr{};
        hr = cpu->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) {
            r.bytes.resize(size_t(desc.Width) * desc.Height * 4);
            for (unsigned y = 0; y < desc.Height; ++y)
                memcpy(r.bytes.data() + size_t(y) * desc.Width * 4,
                       static_cast<BYTE *>(lr.pBits) + y * lr.Pitch, desc.Width * 4);
            cpu->UnlockRect();
        }
        drop(cpu);
        checked(hr, "Lock readback");
        r.config["width"] = desc.Width;
        r.config["height"] = desc.Height;
        r.state = "ready";
    }

  public:
    Engine() {
        worker = std::thread([this] { run(); });
    }
    ~Engine() {
        hud_chat::shutdown();
        profile9::enabled.store(false, std::memory_order_relaxed);
        stop();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(workMutex);
            stopping = true;
            work.clear();
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }
#ifdef HLSL_STANDALONE_TEST
    void testLock() {
        mutex.lock();
    }
    void testUnlock() {
        mutex.unlock();
    }
    void testFailQuery(unsigned failure) {
        testQueryFailure = failure;
    }
    bool testNestedState(IDirect3DDevice9 *d) {
        D3DCAPS9 limits{};
        checked(d->GetDeviceCaps(&limits), "Test caps");
        DWORD original = 0, restoredValue = 0;
        checked(d->GetRenderState(D3DRS_CULLMODE, &original), "Test original state");
        StateCache cache;
        {
            State outer(d, cache, limits);
            checked(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW), "Test outer state");
            {
                State inner(d, cache, limits);
                checked(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW), "Test inner state");
            }
            d->GetRenderState(D3DRS_CULLMODE, &restoredValue);
            if (restoredValue != D3DCULL_CW)
                return false;
        }
        {
            State reused(d, cache, limits);
        }
        d->GetRenderState(D3DRS_CULLMODE, &restoredValue);
        return restoredValue == original && cache.created == 2 && cache.reused == 1;
    }
#endif
    bool updateBytes(unsigned owner, unsigned handle, size_t offset, const char *bytes, size_t size) {
        auto &r = get(owner, handle);
        require(r.kind == "texture" || r.kind == "cube" || r.kind == "volume" || r.kind == "vertices" ||
                    r.kind == "indices",
                "resource is not updatable");
        require(offset <= r.bytes.size() && size <= r.bytes.size() - offset, "update exceeds resource size");
        if (!size)
            return true;
        const auto *first = reinterpret_cast<const BYTE *>(bytes);
        if (std::equal(first, first + size, r.bytes.begin() + offset))
            return true;
        std::copy(first, first + size, r.bytes.begin() + offset);
        r.state = "awaiting_gpu";
        return true;
    }
    std::shared_ptr<const PreparedBatch> prepare(const J &items) {
        require(items.is_array() && !items.empty() && items.size() <= 128, "unsupported prepared frame");
        auto batch = std::make_shared<PreparedBatch>();
        batch->bytes = items.dump().size();
        require(batch->bytes <= 512 * 1024, "prepared frame byte limit");
        batch->items = items;
        batch->passes.resize(items.size());
        std::map<unsigned, std::vector<float>> groups;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto &item = items[i];
            require(item.at("type") == "pass", "unsupported prepared frame");
            const auto &p = item.at("pass");
            require(p.is_object() && !p.value("effect", 0u), "unsupported prepared frame");
            const bool display =
                field(p, "targets", emptyArray).empty() && field(p, "clear", emptyObject).empty();
            if (!display)
                batch->tail = i + 1;
            for (const auto stage : {"ps_constants", "vs_constants"}) {
                const auto &constants = field(p, stage, emptyObject);
                if (constants.empty())
                    continue;
                const auto &blocks = field(constants, "float", emptyArray);
                require(blocks.is_array(), "unsupported prepared constants");
                for (const auto &block : blocks) {
                    const auto group = number(block, "update_group", 0, 4096);
                    require(group, "missing prepared constant group");
                    const auto reg = number(block, "register", 0, 255);
                    auto values = floats(block.at("values"), 1024);
                    require(values.size() % 4 == 0, "unsupported prepared constants");
                    auto found = groups.find(group);
                    if (found != groups.end())
                        require(found->second.size() == values.size() &&
                                    (values.empty() || !memcmp(found->second.data(), values.data(),
                                                               values.size() * sizeof(float))),
                                "prepared constant group mismatch");
                    else
                        groups.emplace(group, std::move(values));
                    batch->passes[i].uniforms.push_back(
                        {reg, group, unsigned(block.at("values").size() / 4), stage[0] == 'v'});
                }
            }
        }
        std::map<unsigned, unsigned> offsets;
        unsigned nextGroup = 1;
        for (const auto &group : groups) {
            require(group.first == nextGroup++, "prepared groups must be consecutive");
            offsets[group.first] = batch->count;
            batch->count += unsigned(group.second.size());
        }
        require(batch->count <= 65536, "prepared constant count limit");
        for (auto &pass : batch->passes)
            for (auto &block : pass.uniforms)
                block.offset = offsets.at(block.offset);
        return batch;
    }
    // Request acceptance is separate from GPU completion. Contention is retryable.
    J request(unsigned owner, const J &req, bool *busy = nullptr, PreparedUpdate *update = nullptr,
              const BufferUpdate *buffer = nullptr) {
        profile9::Scope measured(profile9::BackendRequest);
        if (busy)
            *busy = false;
        auto op = buffer ? std::string("update_raw")
                         : (update ? std::string("submit_prepared") : req.at("op").get<std::string>());
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() && op == "close") {
            std::lock_guard<std::mutex> control(workMutex);
            if (std::find(pendingClose.begin(), pendingClose.end(), owner) == pendingClose.end()) {
                require(pendingClose.size() < 16, "pending close limit");
                pendingClose.push_back(owner);
            }
            return true;
        }
        if (!lock.owns_lock()) {
            ++backendBusyRetries;
            // Expected renderer/API contention must not throw through the host VM.
            if (busy) {
                *busy = true;
                return nullptr;
            }
            throw std::runtime_error("native backend busy; retry next callback");
        }
        flushCloses();
        collect();
        if (op == "open") {
            require(!owners.count(owner) && owners.size() < 16,
                    "owner ID already active or owner limit reached");
            owners[owner] = Owner{};
            return {{"version", "6.4.1"},
                    {"abi", 6},
                    {"prepared_frames", true},
                    {"binary_updates", true},
                    {"persistence_chat", true}};
        }
        require(owners.count(owner), "owner is closed or unknown");
        auto &o = owners[owner];
        if (op == "_persistence_chat") {
            const bool enabled = req.value("enabled", false);
            const bool hide = req.value("hide", false);
            const auto cursor = req.value("since", J(0));
            require(cursor.is_number_integer() && cursor.get<int64_t>() >= 0, "invalid chat cursor");
            auto response = hud_chat::poll(owner, enabled && initialized && !paused, hide,
                                           cursor.get<std::uint64_t>());
            if (enabled && (!initialized || paused))
                response["error"] = "native renderer is not ready for custom chat";
            return response;
        }
        if (op == "close") {
            closeOwner(owner);
            return true;
        }
#ifdef HLSL_STANDALONE_TEST
        if (op == "_test_prepared_values") {
            require(bool(o.preparedFrame), "test requires a pending prepared frame");
            const auto &v = o.preparedFrame->values;
            std::vector<BYTE> bytes(v.size() * sizeof(float));
            if (!bytes.empty())
                memcpy(bytes.data(), v.data(), bytes.size());
            return hex(bytes);
        }
#endif
        if (op == "profiling") {
            const bool value = req.at("enabled").get<bool>();
            if (value != o.profiling) {
                o.profiling = value;
                if (value) {
                    if (++profilingOwners == 1) {
                        profile9::restart();
                        gpuSamples = copySamples = 0;
                        gpuTotalMs = gpuMaxMs = copyTotalMs = copyMaxMs = 0;
                        profile9::enabled.store(true);
                    }
                } else if (--profilingOwners == 0)
                    profile9::enabled.store(false);
            }
            return true;
        }
        if (op == "performance") {
            return {{"scope", "global"},
                    {"enabled", profile9::enabled.load()},
                    {"cpu", profile9::snapshot()},
                    {"state_blocks_created", stateCache.created},
                    {"state_blocks_reused", stateCache.reused},
                    {"queries_created", queriesCreated},
                    {"queries_reused", queriesReused},
                    {"query_failures", queryFailures},
                    {"buffer_creations", bufferCreations},
                    {"buffer_uploads", bufferUploads},
                    {"buffer_uploaded_bytes", bufferUploadedBytes},
                    {"screen_captures", screenCaptures},
                    {"generic_draw_calls", genericDrawCalls},
                    {"clear_calls", clearCalls},
                    {"cached_capture_pixels", size_t(captureW) * captureH},
                    {"cached_state_blocks", stateCache.block ? 1 : 0},
                    {"cached_query_frames",
                     timingPool.size() + timings.size() + (activeTiming.frequency ? 1 : 0)},
                    {"gpu",
                     {{"samples", gpuSamples},
                      {"mean_ms", gpuSamples ? gpuTotalMs / double(gpuSamples) : 0},
                      {"max_ms", gpuMaxMs},
                      {"copy_samples", copySamples},
                      {"copy_mean_ms", copySamples ? copyTotalMs / double(copySamples) : 0},
                      {"copy_max_ms", copyMaxMs}}}};
        }
        if (buffer) {
            const bool result =
                updateBytes(owner, buffer->handle, buffer->offset, buffer->bytes, buffer->size);
            ++o.rawUpdates;
            return result;
        }
        if (op == "prepare_frame") {
            const auto slot = number(req, "slot", 0, 2);
            require(slot >= 1, "prepared slot must be 1 or 2");
            auto batch = prepare(req.at("items"));
            size_t used = batch->bytes;
            for (const auto &entry : owners)
                for (const auto &cached : entry.second.templates)
                    if (entry.first != owner || cached.first != slot)
                        used += cached.second->bytes;
            require(used <= 16 * 1024 * 1024, "prepared frame byte limit");
            o.templates[slot] = batch;
            return {{"slot", slot}, {"count", batch->count}};
        }
        if (update) {
            profile9::Scope measuredSubmit(profile9::Submit);
            const auto it = o.templates.find(update->slot);
            require(it != o.templates.end(), "unknown prepared frame");
            require(update->retain <= 250 && update->values.size() == it->second->count,
                    "prepared update size or retention mismatch");
            require(!update->retain || it->second->tail < it->second->items.size(),
                    "retained frame needs a final backbuffer draw");
            auto submitted = std::make_shared<PreparedSubmission>();
            submitted->batch = it->second;
            submitted->values = std::move(update->values);
            o.frame = nullptr;
            o.presentation = nullptr;
            o.preparedFrame.reset();
            o.preparedPresentation.reset();
            o.preparedFrame = submitted;
            o.preparedPresentation = update->retain ? submitted : nullptr;
            o.retainMs = update->retain;
            o.submitted = GetTickCount64();
            o.error.clear();
            ++o.preparedSubmissions;
            return ++o.sequence;
        }
        if (op == "status") {
            const bool fxAvailable = fx9::available();
            const auto used = cpuUsed();
            J response = {{"initialized", initialized},
                          {"frames", frames},
                          {"profiling_available", true},
                          {"binary_updates", true},
                          {"raw_updates", o.rawUpdates},
                          {"json_updates", o.jsonUpdates},
                          {"prepared_frames", true},
                          {"prepared_submissions", o.preparedSubmissions},
                          {"json_submissions", o.jsonSubmissions},
                          {"prepared_templates", o.templates.size()},
                          {"dynamic_buffers", true},
                          {"buffer_creations", bufferCreations},
                          {"buffer_uploads", bufferUploads},
                          {"buffer_uploaded_bytes", bufferUploadedBytes},
                          {"screen_captures", screenCaptures},
                          {"queries_created", queriesCreated},
                          {"queries_reused", queriesReused},
                          {"query_failures", queryFailures},
                          {"state_blocks_created", stateCache.created},
                          {"state_blocks_reused", stateCache.reused},
                          {"backend_busy_retries", backendBusyRetries.load()},
                          {"frame_retention", true},
                          {"presentation_retention", true},
                          {"retained_draws", o.retainedDraws},
                          {"completed_batch", o.completedSequence},
                          {"resets", resets},
                          {"paused", paused},
                          {"error", o.error},
                          {"gpu_timing", timingSupported},
                          {"fx_runtime", fxAvailable},
                          {"resources", resources.size()},
                          {"cpu_bytes", used},
                          {"vs_version", caps.VertexShaderVersion},
                          {"ps_version", caps.PixelShaderVersion},
                          {"mrt", caps.NumSimultaneousRTs},
                          {"max_streams", caps.MaxStreams},
                          {"max_texture_width", caps.MaxTextureWidth},
                          {"max_texture_height", caps.MaxTextureHeight},
                          {"max_volume_extent", caps.MaxVolumeExtent},
                          {"vertex_texture_filter_caps", caps.VertexTextureFilterCaps}};
            return response;
        }
        if (op == "effect_parameters") {
            auto &r = get(owner, number(req, "handle", 0, 16777215), "effect");
            auto values = req.at("parameters");
            require(values.is_object() || values.empty(), "FX parameters must be an object");
            auto current = r.config.value("parameters", J::object());
            if (current.empty() || req.value("replace", false))
                current = J::object();
            if (!values.empty())
                current.merge_patch(values);
            require(current.size() <= 256 && current.dump().size() <= 262144,
                    "FX parameter block exceeds limits");
            r.config["parameters"] = current;
            return true;
        }
        if (op == "clear") {
            o.frame = nullptr;
            o.presentation = nullptr;
            o.preparedFrame.reset();
            o.preparedPresentation.reset();
            return true;
        }
        if (op == "budget") {
            double ms = req.at("ms").get<double>();
            require(std::isfinite(ms) && ms > 0 && ms <= 100, "budget must be 0 < ms <= 100");
            o.budget = ms;
            return true;
        }
        if (op == "release_all") {
            hud_chat::release(owner);
            std::vector<unsigned> ids;
            for (auto &r : resources)
                if (r.second->owner == owner)
                    ids.push_back(r.first);
            for (auto id : ids)
                erase(owner, id);
            o.frame = nullptr;
            o.presentation = nullptr;
            o.preparedFrame.reset();
            o.preparedPresentation.reset();
            o.templates.clear();
            return true;
        }
        if (op == "release") {
            erase(owner, number(req, "handle", 0, 16777215));
            return true;
        }
        if (op == "info")
            return describe(get(owner, number(req, "handle", 0, 16777215)));
        if (op == "info_many") {
            auto ids = req.at("handles");
            require(ids.is_array() && ids.size() <= 128, "poll accepts at most 128 handles");
            J out = J::array();
            for (auto &id : ids) {
                require(id.is_number_integer(), "integer handle required");
                out.push_back(describe(get(owner, id.get<unsigned>())));
            }
            return out;
        }
        if (op == "compile" || op == "reload") {
            auto opt = req.at("options");
            auto profile = opt.value("profile", std::string("ps_3_0"));
            require(profile == "ps_3_0" || profile == "vs_3_0" || profile == "fx_2_0",
                    "profile must be ps_3_0, vs_3_0 or fx_2_0");
            Resource *r = nullptr;
            if (op == "reload") {
                r = &get(owner, number(req, "handle", 0, 16777215));
                require(r->program(), "not a shader/effect");
                require(r->profile == profile, "reload cannot change shader stage");
            } else {
                r = &allocate(owner, profile == "fx_2_0" ? "effect" : "shader");
                r->profile = profile;
            }
            try {
                auto parameters = (op == "reload" && r->kind == "effect")
                                      ? r->config.value("parameters", J::object())
                                      : J::object();
                queue(*r, opt, r->kind);
                r->config = opt;
                if (r->kind == "effect")
                    r->config["parameters"] = parameters;
            } catch (...) {
                if (op == "compile")
                    erase(owner, r->id);
                throw;
            }
            return r->id;
        }
        if (op == "cancel") {
            auto &r = get(owner, number(req, "handle", 0, 16777215));
            require(r.program(), "not a shader/effect");
            ++r.generation;
            r.pendingCode.clear();
            r.state = r.ps || r.vs || r.effect ? "ready" : "cancelled";
            r.jobState = "cancelled";
            r.error = "compilation cancelled";
            return true;
        }
        if (op == "bytecode") {
            auto opt = req.at("options");
            auto profile = opt.value("profile", std::string("ps_3_0"));
            require(profile == "ps_3_0" || profile == "vs_3_0", "SM3 profile required");
            auto bytes = decodeHex(req.at("hex").get<std::string>(), 65536);
            require(bytes.size() % 4 == 0, "bytecode alignment");
            std::vector<unsigned> code(bytes.size() / 4);
            memcpy(code.data(), bytes.data(), bytes.size());
            std::string error;
            if (!shader6::validate(code, profile == "vs_3_0", error))
                throw std::runtime_error(error);
            auto constants = shader6::reflect(code);
            auto &r = allocate(owner, "shader");
            r.profile = profile;
            r.pendingCode = std::move(code);
            r.pendingConstants = constants;
            ++r.generation;
            return r.id;
        }
        if (op == "readback") {
            unsigned target = number(req, "target", 0, 16777215);
            if (target)
                get(owner, target, "target");
            auto &r = allocate(owner, "readback");
            r.config = {{"target", target}};
            return r.id;
        }
        if (op == "readback_result") {
            auto &r = get(owner, number(req, "handle", 0, 16777215), "readback");
            require(r.state == "ready", "readback not ready; poll info");
            return {{"width", r.config["width"]},
                    {"height", r.config["height"]},
                    {"format", "bgra8"},
                    {"hex", hex(r.bytes)}};
        }
        if (op == "cpu_data") {
            auto &r = get(owner, number(req, "handle", 0, 16777215));
            size_t offset = number(req, "offset", 0, 64 * 1024 * 1024),
                   count = number(req, "count", 0, 1048576);
            require(offset <= r.bytes.size() && count <= r.bytes.size() - offset,
                    "CPU data range outside resource");
            return hex(std::vector<BYTE>(r.bytes.begin() + offset, r.bytes.begin() + offset + count));
        }
        if (op == "float_bits") {
            float f = scalar(req.at("value"));
            unsigned bits = 0;
            memcpy(&bits, &f, 4);
            return bits;
        }
        if (op == "pack") {
            auto values = req.at("values");
            require(values.is_array() && values.size() <= 262144, "pack count limit");
            auto kind = req.value("kind", std::string("float32"));
            require(kind == "float32" || kind == "uint32", "pack kind must be float32 or uint32");
            std::vector<BYTE> bytes(values.size() * 4);
            for (size_t i = 0; i < values.size(); ++i) {
                if (kind == "float32") {
                    float f = scalar(values[i]);
                    memcpy(bytes.data() + i * 4, &f, 4);
                } else {
                    require(values[i].is_number_integer(), "uint32 required");
                    auto n = values[i].get<int64_t>();
                    require(n >= 0 && n <= 0xffffffff, "uint32 range");
                    unsigned word = unsigned(n);
                    memcpy(bytes.data() + i * 4, &word, 4);
                }
            }
            return hex(bytes);
        }
        if (op == "enable_shader") {
            auto &r = get(owner, number(req, "handle", 0, 16777215));
            require(r.program(), "not a shader/effect");
            if (r.disabled) {
                r.disabled = false;
                r.error.clear();
                r.over = 0;
            }
            return true;
        }
        if (op == "create") {
            auto kind = req.at("kind").get<std::string>();
            require(kind == "texture" || kind == "target" || kind == "cube" || kind == "volume" ||
                        kind == "vertices" || kind == "indices" || kind == "layout",
                    "unknown resource kind");
            auto cfg = req.at("config");
            if ((kind == "vertices" || kind == "indices") && cfg.contains("dynamic"))
                require(cfg.at("dynamic").is_boolean(), "dynamic must be boolean");
            std::vector<BYTE> bytes;
            if (cfg.contains("hex"))
                bytes = decodeHex(cfg["hex"].get<std::string>(), 16 * 1024 * 1024);
            else if (kind == "vertices") {
                auto v = floats(cfg.at("data"), 262144);
                bytes.resize(v.size() * 4);
                memcpy(bytes.data(), v.data(), bytes.size());
            } else if (kind == "indices") {
                auto arr = cfg.at("data");
                require(arr.is_array() && arr.size() <= 262144 && !arr.empty(), "index count limit");
                bytes.resize(arr.size() * 4);
                for (unsigned i = 0; i < arr.size(); ++i) {
                    auto n = arr[i].get<int64_t>();
                    require(arr[i].is_number_integer() && n >= 0 && n <= 1048576, "invalid index");
                    unsigned value = unsigned(n);
                    memcpy(bytes.data() + i * 4, &value, 4);
                }
            }
            if (kind == "vertices" || kind == "indices")
                require(!bytes.empty(), "empty geometry buffer");
            cpuBudget(bytes.size(), 0);
            cfg.erase("hex");
            cfg.erase("data");
            auto &r = allocate(owner, kind);
            r.config = cfg;
            r.bytes = std::move(bytes);
            if (kind == "texture" && cfg.contains("path")) {
                try {
                    queue(r, cfg, "texture");
                } catch (...) {
                    erase(owner, r.id);
                    throw;
                }
            }
            return r.id;
        }
        if (op == "update") {
            auto bytes = decodeHex(req.at("hex").get<std::string>(), 16 * 1024 * 1024);
            const bool result = updateBytes(owner, number(req, "handle", 0, 16777215),
                                            number(req, "offset", 0, 64 * 1024 * 1024),
                                            reinterpret_cast<const char *>(bytes.data()), bytes.size());
            ++o.jsonUpdates;
            return result;
        }
        if (op == "resize_target") {
            auto &r = get(owner, number(req, "handle", 0, 16777215), "target");
            unsigned w = number(req, "width", 0, 2048), h = number(req, "height", 0, 2048);
            require(w && h, "target dimensions must be positive");
            r.config["width"] = w;
            r.config["height"] = h;
            r.state = "awaiting_gpu";
            r.defined = false;
            return true;
        }
        if (op == "panel_texture") {
            auto &r = get(owner, number(req, "shader", 0, 16777215), "shader");
            unsigned slot = number(req, "slot", 0, 2), id = number(req, "texture", 0, 16777215);
            if (id)
                get(owner, id);
            r.panelTextures[slot] = id;
            return true;
        }
        if (op == "submit") {
            profile9::Scope measuredSubmit(profile9::Submit);
            auto items = req.at("items");
            require(items.is_array() && items.size() <= 128, "frame item limit 128");
            const unsigned retain = number(req, "retain_ms", 0, 250);
            J presentation = J::array();
            if (retain) {
                // Only the final backbuffer drawing tail may repeat. Any RT,
                // effect or clear command starts a new one-shot dependency chain.
                for (const auto &item : items) {
                    const auto type = item.at("type").get<std::string>();
                    bool display = type == "panels";
                    if (type == "pass") {
                        const auto &p = item.at("pass");
                        display = p.value("targets", J::array()).empty() && !p.value("effect", 0u) &&
                                  p.value("clear", J::object()).empty();
                    }
                    if (display)
                        presentation.push_back(item);
                    else
                        presentation.clear();
                }
                require(!presentation.empty(), "retained frame needs a final backbuffer draw");
            }
            o.preparedFrame.reset();
            o.preparedPresentation.reset();
            ++o.jsonSubmissions;
            o.frame = std::move(items);
            o.presentation = retain ? std::move(presentation) : J(nullptr);
            o.retainMs = retain;
            o.submitted = GetTickCount64();
            o.error.clear();
            ++o.sequence;
            return o.sequence;
        }
        throw std::runtime_error("unknown API operation");
    }
    static bool captureValue(const J &value) {
        if (value.is_object()) {
            const auto it = value.find("capture");
            if (it != value.end() && it->is_boolean() && it->get<bool>())
                return true;
            for (const auto &child : value)
                if (captureValue(child))
                    return true;
        } else if (value.is_array()) {
            for (const auto &child : value)
                if (captureValue(child))
                    return true;
        }
        return false;
    }
    bool needsCapture(unsigned owner, const J &items, size_t first = 0) {
        for (size_t index = first; index < items.size(); ++index) {
            const auto &item = items[index];
            if (!item.is_object())
                continue; // The draw path reports malformed commands.
            const auto type = item.value("type", std::string());
            if (type == "panels" && !item.at("panels").empty())
                return true;
            if (type != "pass")
                continue;
            const auto &p = item.at("pass");
            if (!p.is_object() || p.value("clear_only", false))
                continue;
            if (captureValue(field(p, "textures", emptyArray)) ||
                captureValue(field(p, "parameters", emptyObject)))
                return true;
            const auto effect = p.find("effect");
            if (effect != p.end() && effect->is_number_unsigned()) {
                const auto it = resources.find(effect->get<unsigned>());
                if (it != resources.end() && it->second->owner == owner &&
                    captureValue(field(it->second->config, "parameters", emptyObject)))
                    return true;
            }
        }
        return false;
    }
    void render(IDirect3DDevice9 *d) {
        profile9::Scope measured(profile9::Render);
        const auto lockStarted = measured.active ? profile9::now() : 0;
        std::lock_guard<std::mutex> lock(mutex);
        if (measured.active)
            profile9::record(profile9::RenderLock, profile9::now() - lockStarted);
        flushCloses();
        if (owners.empty()) {
            retire();
            stateCache.reset();
            drop(quadDeclaration);
            drop(captureSurface);
            drop(capture);
            captureW = captureH = 0;
            for (auto &frame : timings)
                queryDrop(frame);
            for (auto &frame : timingPool)
                queryDrop(frame);
            timings.clear();
            timingPool.clear();
            queryDrop(activeTiming);
            activeTiming = {};
            openTiming = SIZE_MAX;
            timingSupported = false;
            panels.reset();
            return;
        }
        if (paused || FAILED(d->TestCooperativeLevel())) {
            retire();
            return;
        }
        restoreFailure = S_OK;
        if (capsDevice != d) {
            stateCache.reset();
            checked(d->GetDeviceCaps(&caps), "GetDeviceCaps");
            capsDevice = d;
        }
        initialized = true;
        collect();
        retire();
        for (auto &v : resources) {
            auto &r = *v.second;
            if (r.state == "awaiting_gpu" && r.kind != "readback")
                try {
                    createGPU(d, r);
                } catch (const std::exception &e) {
                    r.pendingCode.clear();
                    r.state = "failed";
                    if (r.program())
                        r.jobState = "failed";
                    r.error = e.what();
                }
        }
        timingPoll();
        bool any = false;
        for (auto &o : owners)
            if (!o.second.frame.is_null() || !o.second.presentation.is_null() || o.second.preparedFrame ||
                o.second.preparedPresentation)
                any = true;
        if (any)
            timingBegin(d);
        bool captured = false, captureRequired = false;
        // Discover dependencies across every owner before the first overlay draw,
        // so a later owner always samples the original pre-overlay backbuffer.
        for (const auto &entry : owners) {
            const auto &o = entry.second;
            const bool fresh = !o.frame.is_null() || bool(o.preparedFrame);
            const auto age = GetTickCount64() - o.submitted;
            if ((fresh && age > 250) || (!fresh && (!o.retainMs || age > o.retainMs)))
                continue;
            const auto prepared = fresh ? o.preparedFrame : o.preparedPresentation;
            const auto &items = prepared ? prepared->batch->items : (fresh ? o.frame : o.presentation);
            if (items.is_null())
                continue;
            try {
                if (needsCapture(entry.first, items, prepared && !fresh ? prepared->batch->tail : 0)) {
                    captureRequired = true;
                    break;
                }
            } catch (...) { /* Preserve normal per-owner execution error handling. */
            }
        }
        for (auto &entry : owners) {
            auto &o = entry.second;
            const bool fresh = !o.frame.is_null() || bool(o.preparedFrame);
            J consumed;
            const J *items = nullptr;
            auto prepared = fresh ? std::move(o.preparedFrame) : o.preparedPresentation;
            size_t firstItem = 0;
            const auto age = GetTickCount64() - o.submitted;
            if (fresh) {
                if (!prepared)
                    consumed = std::move(o.frame);
                o.frame = nullptr;
                if (age > 250) {
                    o.presentation = nullptr;
                    o.preparedFrame.reset();
                    o.preparedPresentation.reset();
                    continue;
                }
                items = prepared ? &prepared->batch->items : &consumed;
            } else {
                if (!prepared && o.presentation.is_null())
                    continue;
                if (!o.retainMs || age > o.retainMs) {
                    o.presentation = nullptr;
                    o.preparedFrame.reset();
                    o.preparedPresentation.reset();
                    continue;
                }
                items = prepared ? &prepared->batch->items : &o.presentation;
                if (prepared)
                    firstItem = prepared->batch->tail;
            }
            try {
                if (captureRequired && !captured) {
                    captureFrame(d);
                    if (activeTiming.profiled) {
                        activeTiming.copyMeasured = SUCCEEDED(activeTiming.copyEnd->Issue(D3DISSUE_END));
                        if (!activeTiming.copyMeasured) {
                            ++queryFailures;
                            drop(activeTiming.copyEnd);
                        }
                    }
                    captured = true;
                }
                for (size_t index = firstItem; index < items->size(); ++index) {
                    const auto &item = (*items)[index];
                    auto type = item.at("type").get<std::string>();
                    if (type == "panels")
                        drawPanels(d, entry.first, item.at("panels"));
                    else if (type == "pass")
                        pass(d, entry.first, item.at("pass"),
                             prepared ? &prepared->batch->passes[index] : nullptr,
                             prepared ? &prepared->values : nullptr);
                    else
                        throw std::runtime_error("unknown frame item");
                }
                if (fresh) {
                    o.completedSequence = o.sequence;
                    ++frames;
                } else {
                    ++o.retainedDraws;
                }
            } catch (const std::exception &e) {
                o.frame = nullptr;
                o.presentation = nullptr;
                o.preparedFrame.reset();
                o.preparedPresentation.reset();
                o.error = e.what();
            }
        }
        timingEnd();
        for (auto &v : resources) {
            auto &r = *v.second;
            if (r.kind == "readback" && r.state == "awaiting_gpu")
                try {
                    readback(d, r);
                } catch (const std::exception &e) {
                    r.state = "failed";
                    r.error = e.what();
                }
        }
        if (FAILED(restoreFailure)) {
            paused = true;
            for (auto &o : owners)
                o.second.error = "device state restoration failed; rendering paused until device reset";
        }
        flushCloses();
        retire();
    }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        flushCloses();
        paused = false;
        stateCache.reset();
        capsDevice = nullptr;
        drop(quadDeclaration);
        panels.reset();
        drop(captureSurface);
        drop(capture);
        captureW = captureH = 0;
        for (auto &f : timings)
            queryDrop(f);
        timings.clear();
        for (auto &frame : timingPool)
            queryDrop(frame);
        timingPool.clear();
        openTiming = SIZE_MAX;
        queryDrop(activeTiming);
        activeTiming = {};
        retire();
        for (auto &v : resources) {
            auto &r = *v.second;
            r.gpu();
            r.defined = false;
            if (r.state == "ready" || (r.program() && !r.code.empty() && r.pendingCode.empty()))
                r.state = "awaiting_gpu";
        }
        for (auto &o : owners) {
            o.second.frame = nullptr;
            o.second.presentation = nullptr;
            o.second.preparedFrame.reset();
            o.second.preparedPresentation.reset();
        }
        ++resets;
    }
};
// Deliberate process-lifetime service: no std::thread destructor under loader lock.
Engine *&instance() {
    static Engine *p = nullptr;
    return p;
}
std::mutex initMutex;
Engine &engine() {
    std::lock_guard<std::mutex> lock(initMutex);
    if (!instance())
        instance() = new Engine;
    return *instance();
}
} // namespace
json request(unsigned owner, const json &j) {
    return engine().request(owner, j);
}
bool try_update(unsigned owner, const BufferUpdate &update, json &result) {
    bool busy = false;
    result = engine().request(owner, emptyObject, &busy, nullptr, &update);
    return !busy;
}
bool try_submit(unsigned owner, PreparedUpdate &update, json &result) {
    bool busy = false;
    result = engine().request(owner, emptyObject, &busy, &update);
    return !busy;
}
bool try_request(unsigned owner, const json &j, json &result) {
    bool busy = false;
    result = engine().request(owner, j, &busy);
    return !busy;
}
#ifdef HLSL_STANDALONE_TEST
void test_fail_query(unsigned failure) {
    engine().testFailQuery(failure);
}
bool test_nested_state(IDirect3DDevice9 *d) {
    return engine().testNestedState(d);
}
void test_lock_backend() {
    engine().testLock();
}
void test_unlock_backend() {
    engine().testUnlock();
}
#endif
void render(IDirect3DDevice9 *d) {
    engine().render(d);
}
void reset() {
    if (instance())
        instance()->reset();
}
void shutdown() {
    if (instance()) {
        instance()->reset();
        instance()->stop();
        delete instance();
        instance() = nullptr;
    }
}
} // namespace dx9_api
