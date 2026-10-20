#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <cassert>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <iostream>
#include <misc/TemporalSampling.h>
#include <misc/MipBiasPolicy.h>
#include <misc/SamplerMipBias.h>
#include <misc/NgxJitterBinding.h>

static float capturedBias=0;static unsigned created=0;static bool failCreate=false;
static auto o_D3D12SerializeRootSignature=&::D3D12SerializeRootSignature;
static auto o_D3D12SerializeVersionedRootSignature=&::D3D12SerializeVersionedRootSignature;
namespace D3d12Proxy {inline auto D3D12CreateVersionedRootSignatureDeserializer_(){return &::D3D12CreateVersionedRootSignatureDeserializer;}}
static std::shared_mutex rootSigParameterCountMutex;
static std::map<ID3D12RootSignature*,UINT> rootSigParameterCount;
static D3D12_FILTER UpgradeToAF(D3D12_FILTER f){return f;}
static D3D11_FILTER UpgradeToAF(D3D11_FILTER f){return f;}
HRESULT CaptureRoot(ID3D12Device*,UINT,const void* data,SIZE_T size,REFIID,void** out) {
    ++created;ID3D12VersionedRootSignatureDeserializer* parser=nullptr;
    auto hr=D3D12CreateVersionedRootSignatureDeserializer(data,size,IID_PPV_ARGS(&parser));assert(SUCCEEDED(hr));
    auto desc=parser->GetUnconvertedRootSignatureDesc();
    capturedBias=desc->Version==D3D_ROOT_SIGNATURE_VERSION_1_0?desc->Desc_1_0.pStaticSamplers[0].MipLODBias:desc->Desc_1_1.pStaticSamplers[0].MipLODBias;
    parser->Release();if(out)*out=failCreate?nullptr:reinterpret_cast<void*>(0x1234);
    return failCreate?E_FAIL:S_OK;
}
static auto o_CreateRootSignature=&CaptureRoot;
HRESULT CaptureSampler11(ID3D11Device*,const D3D11_SAMPLER_DESC* d,ID3D11SamplerState**) {capturedBias=d->MipLODBias;return S_OK;}
static auto o_CreateSamplerState=&CaptureSampler11;
static bool UseAutomaticMipBias(){return false;}
void CaptureSampler12(ID3D12Device*,const D3D12_SAMPLER_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE) {capturedBias=d->MipLODBias;}
static auto o_CreateSampler=&CaptureSampler12;
#include "sampler_production.inl"

void Near(float a,float b){assert(std::abs(a-b)<1e-5f);}
int main() {
    using namespace MipBiasPolicy;
    Near(Recommended(1920,3840,0,0),-2);Near(Recommended(1920,3840,0,1),-1);
    Near(Recommended(2560,3840,-.25f,.5f),-.25f+std::log2(2.f/3)-.5f);
    Near(Recommended(0,3840),0);
    auto inf=std::numeric_limits<float>::infinity();
    Near(Apply(-1,0,16,0x15,-.25f,false,false,false),-1.25f);
    Near(Apply(-1,0,16,0x15,-.25f,true,false,false),-.25f);
    Near(Apply(-1,0,16,0x15,2.f,false,true,false),-2);
    for(unsigned filter:{0x80u,0x100u,0x180u})Near(Apply(-1,0,16,filter,-1.f,false,false,false),-1);
    Near(Apply(-1,0,0,0x15,-1.f,false,false,false),-1);
    Near(Apply(0,0,16,0x55,-1.f,false,false,false),0);
    Near(Apply(-1,0,16,0x15,inf,false,false,false),-1);
    Near(Apply(-1,0,16,0x15,std::nullopt,false,false,false),-1);
    Near(Apply(-10,0,16,0x15,-10.f,false,false,false),-16);
    Near(Apply(0,0,0,0x80,-1.f,true,false,true),-1); // explicit all remains available
    auto& c=*Config::Instance();c.MipmapBiasOverride=-.25f;
    D3D11_SAMPLER_DESC d11{};d11.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;d11.MipLODBias=-1;d11.MaxLOD=16;
    auto device11=reinterpret_cast<ID3D11Device*>(1);assert(SUCCEEDED(hkCreateSamplerState(device11,&d11,nullptr)));Near(capturedBias,-1.25f);Near(d11.MipLODBias,-1);
    D3D12_SAMPLER_DESC d12{};d12.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;d12.MipLODBias=-1;d12.MaxLOD=16;
    auto device12=reinterpret_cast<ID3D12Device*>(1);hkCreateSampler(device12,&d12,{});Near(capturedBias,-1.25f);Near(d12.MipLODBias,-1);
    D3D12_STATIC_SAMPLER_DESC1 static12{};static12.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;static12.MipLODBias=-1;static12.MaxLOD=16;
    ApplySamplerOverrides(static12);Near(static12.MipLODBias,-1.25f);
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy=1;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=16;sampler.MipLODBias=-1;
    for(bool versioned:{false,true}) {
        ID3DBlob* blob=nullptr;ID3DBlob* error=nullptr;
        D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        D3D12_ROOT_SIGNATURE_DESC desc{1,&parameter,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_ROOT_PARAMETER1 parameter1{};parameter1.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC v{};v.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;v.Desc_1_1={1,&parameter1,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        auto hr=versioned?hkD3D12SerializeVersionedRootSignature(&v,&blob,&error):hkD3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&error);
        assert(SUCCEEDED(hr));if(error)error->Release();Near(sampler.MipLODBias,-1);
        ID3D12VersionedRootSignatureDeserializer* parser=nullptr;
        assert(SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&parser))));
        auto unmodified=parser->GetUnconvertedRootSignatureDesc();Near(versioned?unmodified->Desc_1_1.pStaticSamplers[0].MipLODBias:unmodified->Desc_1_0.pStaticSamplers[0].MipLODBias,-1);parser->Release();
        void* root=nullptr;rootSigParameterCount.clear();
        assert(SUCCEEDED(hkCreateRootSignature(device12,0,blob->GetBufferPointer(),blob->GetBufferSize(),__uuidof(ID3D12RootSignature),&root)));
        Near(capturedBias,-1.25f);assert(created>0);assert(!rootSigParameterCount.contains(nullptr));assert(rootSigParameterCount.at(static_cast<ID3D12RootSignature*>(root))==1);
        failCreate=true;rootSigParameterCount.clear();assert(FAILED(hkCreateRootSignature(device12,0,blob->GetBufferPointer(),blob->GetBufferSize(),__uuidof(ID3D12RootSignature),&root)));assert(rootSigParameterCount.empty());failCreate=false;
        blob->Release();
    }
    using namespace TemporalSampling;
    assert(PhaseCount(2258,1270,3840,2160)==24);assert(PhaseCount(2227,1253,3840,2160)==24);
    assert(PhaseCount(1920,1080,3840,2160)==32);assert(PhaseCount(2560,1440,3840,2160)==18);
    assert(PhaseCount(1280,720,3840,2160)==72);assert(PhaseCount(3840,2160,3840,2160)==8);
    assert(PhaseCount(1,1,0xffffffff,0xffffffff)==4096);
    for(unsigned i=1;i<=72;++i)for(unsigned base:{2u,3u})assert(std::abs(Halton(i,base))<=.5f);
    NVSDK_NGX_Parameter p;p.x=.4f;p.y=-.3f;
    try {NgxJitterBinding::Resampled resized(&p,3840,2160,1920,1080);Near(p.x,.2f);Near(p.y,-.15f);throw 1;}catch(int){}
    Near(p.x,.4f);Near(p.y,-.3f);
    {auto writes=p.writes;NgxJitterBinding::Resampled native(&p,0,0,2560,1440);assert(p.writes==writes);}
    p.haveX=false;{NgxJitterBinding::Resampled partial(&p,3840,2160,2560,1440);Near(p.x,.4f);Near(p.y,-.2f);}Near(p.y,-.3f);
    {NgxJitterBinding::Resampled absent(nullptr,3840,2160,1920,1080);}
    std::cout<<"PASS: production sampler hooks, DX12 serialization/create once, failed-create bookkeeping, bias modes/guards, NVIDIA formula, jitter phases and temporary resample units/restoration\n";
}
