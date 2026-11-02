#pragma once
#include <Windows.h>
#include <d3d9.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

// One draw. `params` maps to shader constants c6..c13 (p0..p7).
// `shader` selects a registered user shader; 0 falls back to `mode`.
struct HlslPanel {
    float x, y, width, height, radius, blur, refraction, opacity;
    float tint[4];
    float edge, dispersion, noise, saturation, brightness;
    float params[32];
    int mode;   // builtin material 0..2
    int blend;  // 0 straight alpha, 1 additive
    int shader; // registry handle, 0 = builtin
};
static_assert(sizeof(HlslPanel) == 208, "Lua ABI mismatch");

class HlslRenderer {
    struct Custom {
        IDirect3DPixelShader9 *ps = nullptr;
        unsigned generation = 0;
        std::vector<unsigned int> code; // retained for device recreation
        IDirect3DTexture9 *textures[3] = {};
        std::wstring texture_paths[3];
        double ema = 0.0;
        unsigned over = 0, draws = 0;
        bool disabled = false;
    };
    IDirect3DTexture9 *capture_ = nullptr;
    IDirect3DSurface9 *surface_ = nullptr;
    IDirect3DPixelShader9 *shaders_[3] = {};
    std::vector<unsigned int> custom_[3];
    std::map<int, Custom> users_;
    UINT width_ = 0, height_ = 0;
    D3DFORMAT format_ = D3DFMT_UNKNOWN;
    double budgetMs_ = 2.0;
    unsigned rejected_ = 0;
    HRESULT ensure(IDirect3DDevice9 *, const D3DSURFACE_DESC &, bool);
    Custom *find(int h);

  public:
    static constexpr unsigned kBudgetOverFrames = 90;
    std::function<void(int, bool)> timing;
    void bind_texture(int h, int slot, IDirect3DTexture9 *t);
    ~HlslRenderer() {
        reset();
    }
    void reset();
    void clear_custom() {
        for (auto &c : custom_)
            c.clear();
    }
    // Must be called outside BeginScene/EndScene on the device's render thread.
    HRESULT render(IDirect3DDevice9 *, const HlslPanel *, UINT, float, IDirect3DSurface9 *source = nullptr,
                   IDirect3DTexture9 *sharedCapture = nullptr);
    // Builtin material slot 0..2.
    HRESULT install_material(IDirect3DDevice9 *, int, const std::vector<unsigned int> &);
    // User handle: atomic replace, bytecode retained, failure keeps the old one.
    HRESULT install_user(IDirect3DDevice9 *, int, std::vector<unsigned int>, unsigned);
    bool alive(int h) const {
        return users_.count(h) != 0;
    }
    bool disabled(int h) const;
    unsigned generation(int h) const;
    double frame_ms(int h) const;
    void release_user(int h);
    void clear_users() {
        users_.clear();
    }
    void set_budget(double ms) {
        budgetMs_ = ms > 0 ? ms : 2.0;
    }
    unsigned rejected() const {
        return rejected_;
    }
    static bool valid(const HlslPanel &);
};
