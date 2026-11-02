#include "hlsl_renderer.h"
#include "dds_loader.h"
#include "material_chromatic.h"
#include "material_frosted.h"
#include "material_liquid.h"
#include <algorithm>
#include <cmath>
#include <cstring>

template <class T> static void release(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

HlslRenderer::Custom *HlslRenderer::find(int h) {
    auto it = users_.find(h);
    return it == users_.end() ? nullptr : &it->second;
}
bool HlslRenderer::disabled(int h) const {
    auto it = users_.find(h);
    return it != users_.end() && it->second.disabled;
}
unsigned HlslRenderer::generation(int h) const {
    auto it = users_.find(h);
    return it == users_.end() ? 0 : it->second.generation;
}
double HlslRenderer::frame_ms(int h) const {
    auto it = users_.find(h);
    return it == users_.end() ? 0.0 : it->second.ema;
}

HRESULT HlslRenderer::install_user(IDirect3DDevice9 *d, int h, std::vector<unsigned int> code,
                                   unsigned generation) {
    if (!d || h <= 0 || code.empty())
        return E_INVALIDARG;
    IDirect3DPixelShader9 *ps = nullptr;
    HRESULT hr = d->CreatePixelShader(reinterpret_cast<const DWORD *>(code.data()), &ps);
    if (FAILED(hr))
        return hr; // previous program stays bound
    Custom &c = users_[h];
    release(c.ps);
    c.ps = ps;
    c.code = std::move(code);
    c.generation = generation;
    c.disabled = false;
    c.over = 0;
    c.ema = 0.0;
    return S_OK;
}

void HlslRenderer::release_user(int h) {
    auto it = users_.find(h);
    if (it == users_.end())
        return;
    release(it->second.ps);
    for (auto &t : it->second.textures)
        release(t);
    users_.erase(it);
}

void HlslRenderer::reset() {
    release(surface_);
    release(capture_);
    for (auto &p : shaders_)
        release(p);
    for (auto &kv : users_) { // bytecode survives, GPU objects do not
        release(kv.second.ps);
        for (auto &t : kv.second.textures)
            release(t);
    }
    width_ = height_ = 0;
    format_ = D3DFMT_UNKNOWN;
}

bool HlslRenderer::valid(const HlslPanel &p) {
    float f[49];
    std::memcpy(f, &p, sizeof(f));
    for (int i = 0; i < 49; ++i)
        if (!std::isfinite(f[i]))
            return false;
    return p.width > 0 && p.height > 0 && p.width <= 16384 && p.height <= 16384 && std::abs(p.x) <= 32768 &&
           std::abs(p.y) <= 32768 && p.radius >= 0 && p.radius <= 8192 && p.blur >= 0 && p.blur <= 32 &&
           p.refraction >= 0 && p.refraction <= 64 && p.opacity >= 0 && p.opacity <= 1 && p.tint[3] >= 0 &&
           p.tint[3] <= 1 && p.tint[0] >= 0 && p.tint[0] <= 1 && p.tint[1] >= 0 && p.tint[1] <= 1 &&
           p.tint[2] >= 0 && p.tint[2] <= 1 && p.edge >= 0 && p.edge <= 2 && p.dispersion >= 0 &&
           p.dispersion <= 16 && p.noise >= 0 && p.noise <= 0.2 && p.saturation >= 0 && p.saturation <= 2 &&
           p.brightness >= 0 && p.brightness <= 2 && p.mode >= 0 && p.mode < 3 &&
           (p.blend == 0 || p.blend == 1) && p.shader >= 0;
}

HRESULT HlslRenderer::ensure(IDirect3DDevice9 *d, const D3DSURFACE_DESC &desc, bool ownCapture) {
    if (width_ == desc.Width && height_ == desc.Height && format_ == desc.Format && shaders_[0] &&
        shaders_[1] && shaders_[2] && (!ownCapture || capture_))
        return S_OK;
    if (width_ != desc.Width || height_ != desc.Height || format_ != desc.Format) {
        release(surface_);
        release(capture_);
    }
    D3DCAPS9 caps{};
    HRESULT hr = d->GetDeviceCaps(&caps);
    if (FAILED(hr))
        return hr;
    if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
        return D3DERR_NOTAVAILABLE;
    if (ownCapture && !capture_) {
        hr = d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
                              &capture_, nullptr);
        if (SUCCEEDED(hr))
            hr = capture_->GetSurfaceLevel(0, &surface_);
    }
    const BYTE *code[3] = {material_liquid, material_frosted, material_chromatic};
    for (int i = 0; i < 3 && SUCCEEDED(hr); ++i)
        if (!shaders_[i])
            hr = d->CreatePixelShader(custom_[i].empty() ? reinterpret_cast<const DWORD *>(code[i])
                                                         : reinterpret_cast<const DWORD *>(custom_[i].data()),
                                      &shaders_[i]);
    if (FAILED(hr)) {
        reset();
        return hr;
    }
    width_ = desc.Width;
    height_ = desc.Height;
    format_ = desc.Format;
    return S_OK;
}

HRESULT HlslRenderer::install_material(IDirect3DDevice9 *d, int slot, const std::vector<unsigned int> &code) {
    if (!d || slot < 0 || slot > 2 || code.empty())
        return E_INVALIDARG;
    IDirect3DPixelShader9 *shader = nullptr;
    HRESULT hr = d->CreatePixelShader(reinterpret_cast<const DWORD *>(code.data()), &shader);
    if (FAILED(hr))
        return hr;
    custom_[slot] = code;
    release(shaders_[slot]);
    shaders_[slot] = shader;
    return S_OK;
}

HRESULT HlslRenderer::render(IDirect3DDevice9 *d, const HlslPanel *panels, UINT count, float time,
                             IDirect3DSurface9 *source, IDirect3DTexture9 *sharedCapture) {
    if (!d || !panels || count > 64 || !std::isfinite(time))
        return E_INVALIDARG;
    if (!count)
        return S_OK;
    // One bad panel must not blank the frame: skip it and draw the rest.
    HlslPanel keep[64];
    UINT n = 0;
    for (UINT i = 0; i < count; ++i) {
        if (valid(panels[i]))
            keep[n++] = panels[i];
        else
            ++rejected_;
    }
    if (!n)
        return S_OK;
    panels = keep;
    count = n;
    HRESULT hr = d->TestCooperativeLevel();
    if (FAILED(hr))
        return hr;
    IDirect3DSurface9 *back = nullptr;
    hr = d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (FAILED(hr))
        return hr;
    D3DSURFACE_DESC desc = {};
    hr = back->GetDesc(&desc);
    if (SUCCEEDED(hr))
        hr = ensure(d, desc, sharedCapture == nullptr);
    // StretchRect stays outside the scene. All panels share one capture.
    if (SUCCEEDED(hr) && !sharedCapture)
        hr = d->StretchRect(source ? source : back, nullptr, surface_, nullptr, D3DTEXF_NONE);
    if (FAILED(hr)) {
        release(back);
        return hr;
    }
    IDirect3DStateBlock9 *state = nullptr;
    hr = d->CreateStateBlock(D3DSBT_ALL, &state);
    if (SUCCEEDED(hr))
        hr = state->Capture();
    if (FAILED(hr)) {
        release(state);
        release(back);
        return hr;
    }
    D3DVIEWPORT9 viewport = {};
    RECT scissor = {};
    UINT streamFrequency = 1;
    d->GetStreamSourceFreq(0, &streamFrequency);
    d->GetViewport(&viewport);
    d->GetScissorRect(&scissor);
    IDirect3DSurface9 *targets[4] = {};
    IDirect3DSurface9 *depth = nullptr;
    d->GetDepthStencilSurface(&depth);
    D3DCAPS9 caps = {};
    d->GetDeviceCaps(&caps);
    UINT targetCount = std::min<UINT>(caps.NumSimultaneousRTs, 4);
    for (UINT i = 0; i < targetCount; ++i)
        d->GetRenderTarget(i, &targets[i]);
    hr = d->BeginScene();
    if (SUCCEEDED(hr)) {
        d->SetDepthStencilSurface(nullptr);
        hr = d->SetRenderTarget(0, back);
        for (UINT i = 1; i < targetCount; ++i)
            d->SetRenderTarget(i, nullptr);
        D3DVIEWPORT9 screen = {0, 0, desc.Width, desc.Height, 0, 1};
        d->SetViewport(&screen);
        d->SetVertexShader(nullptr);
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        d->SetStreamSourceFreq(0, 1);
        d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_LIGHTING, FALSE);
        d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
        d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
        d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        d->SetTexture(0, sharedCapture ? sharedCapture : capture_);
        for (int s = 1; s < 4; ++s) {
            d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        const float scr[4] = {float(desc.Width), float(desc.Height), 1.f / desc.Width, 1.f / desc.Height};
        d->SetPixelShaderConstantF(0, scr, 1);
        struct Vertex {
            float x, y, z, rhw, u, v;
        };
        IDirect3DPixelShader9 *last = nullptr;
        int lastBlend = -1;
        for (UINT i = 0; i < count && SUCCEEDED(hr); ++i) {
            const auto &p = panels[i];
            IDirect3DPixelShader9 *ps = nullptr;
            Custom *user = nullptr;
            if (p.shader > 0) {
                user = find(p.shader);
                if (user && !user->disabled) {
                    if (!user->ps && !user->code.empty())
                        d->CreatePixelShader(reinterpret_cast<const DWORD *>(user->code.data()), &user->ps);
                    ps = user->ps;
                }
            }
            if (!ps)
                ps = shaders_[p.mode];
            if (ps != last) {
                d->SetPixelShader(ps);
                last = ps;
            }
            if (p.blend != lastBlend) {
                const D3DBLEND dst = p.blend ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA;
                d->SetRenderState(D3DRS_DESTBLEND, dst);
                d->SetRenderState(D3DRS_DESTBLENDALPHA, dst);
                lastBlend = p.blend;
            }
            const float rect[4] = {p.x, p.y, p.width, p.height};
            const float shape[4] = {p.radius, p.blur, p.refraction, p.opacity};
            const float finish[4] = {p.edge, p.dispersion, p.noise, time};
            const float tone[4] = {p.saturation, p.brightness, 0, 0};
            d->SetPixelShaderConstantF(1, rect, 1);
            d->SetPixelShaderConstantF(2, shape, 1);
            d->SetPixelShaderConstantF(3, p.tint, 1);
            d->SetPixelShaderConstantF(4, finish, 1);
            d->SetPixelShaderConstantF(5, tone, 1);
            if (user) {
                d->SetPixelShaderConstantF(6, p.params, 8);
                for (int s = 0; s < 3; ++s) {
                    d->SetTexture(1 + s, user->textures[s]);
                }
            }
            const float x = p.x - .5f, y = p.y - .5f;
            Vertex v[4] = {{x, y, 0, 1, 0, 0},
                           {x + p.width, y, 0, 1, 1, 0},
                           {x, y + p.height, 0, 1, 0, 1},
                           {x + p.width, y + p.height, 0, 1, 1, 1}};
            if (timing)
                timing(p.shader, true);
            hr = d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
            if (timing)
                timing(p.shader, false);
        }
        for (int s = 1; s < 4; ++s)
            d->SetTexture(s, nullptr);
        d->SetTexture(0, nullptr);
        HRESULT end = d->EndScene();
        if (SUCCEEDED(hr))
            hr = end;
    }
    for (UINT i = 1; i < targetCount; ++i)
        d->SetRenderTarget(i, nullptr);
    for (UINT i = 0; i < targetCount; ++i) {
        d->SetRenderTarget(i, targets[i]);
        release(targets[i]);
    }
    d->SetDepthStencilSurface(depth);
    release(depth);
    HRESULT restored = state->Apply();
    d->SetViewport(&viewport);
    d->SetScissorRect(&scissor);
    const auto frequencyRestored = d->SetStreamSourceFreq(0, streamFrequency);
    if (SUCCEEDED(restored))
        restored = frequencyRestored;
    if (SUCCEEDED(hr))
        hr = restored;
    release(state);
    release(back);
    return hr;
}

void HlslRenderer::bind_texture(int h, int slot, IDirect3DTexture9 *t) {
    auto c = find(h);
    if (!c || slot < 0 || slot > 2)
        return;
    if (t)
        t->AddRef();
    release(c->textures[slot]);
    c->textures[slot] = t;
    c->texture_paths[slot].clear();
}
