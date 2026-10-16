#pragma once
#include <d3d11.h>
#include <atomic>
#include <memory>
namespace FfxivPreviewNR {
inline std::atomic<unsigned long long> evaluated{0},skipped{0};
inline std::atomic<unsigned> instances{0};
inline std::atomic<unsigned long long> bottleneckComputed{0},bottleneckReused{0};
class Instance {
    struct Impl;Impl* p;
public:
    Instance();~Instance();
    Instance(const Instance&)=delete;
    bool Process(ID3D11DeviceContext*,ID3D11Texture2D* colour,ID3D11Texture2D* depth,ID3D11Texture2D* motion,bool inverted,bool reset,unsigned long long frameId);
    bool Ready() const;
    ID3D11ShaderResourceView* InputView() const;
    void Stop(UINT64 deadline);
};
void Retire(std::unique_ptr<Instance>);
void Collect();
void Suspend();
void Resume();
}
