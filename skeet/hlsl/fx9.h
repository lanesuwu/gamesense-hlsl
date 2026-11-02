#pragma once
#include "compiler6.h"
#include <d3d9.h>
#include <functional>
#include <memory>
namespace fx9 {
bool available();
shader6::Result compile(const shader6::json &, const std::filesystem::path &);
class Effect {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Effect(IDirect3DDevice9 *, const std::vector<unsigned> &);
    ~Effect();
    shader6::json describe() const;
    unsigned passes(const std::string &) const;
    void start(const std::string &, unsigned, const shader6::json &,
               const std::function<IDirect3DBaseTexture9 *(const shader6::json &)> &);
    void next(unsigned);
    void end() noexcept;
};
} // namespace fx9
