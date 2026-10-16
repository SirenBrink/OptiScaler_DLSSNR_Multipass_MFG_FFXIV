#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace FfxivPreviewCamera {
using Microsoft::WRL::ComPtr;
struct Snapshot {
    ComPtr<ID3D11Buffer> buffer;
    UINT bytes=0,first=0,count=0;
};
// The reflected camera layout occupies 944 bytes, but the game's allocator
// may pad the resource or bind a slice of a larger constant buffer. Copy the
// complete resource (partial constant-buffer copies are not supported) and
// retain the native binding range. Never modify the game's camera buffer.
inline bool Capture(ID3D11DeviceContext* context,Snapshot& snapshot) {
    ComPtr<ID3D11DeviceContext1> c;
    if(FAILED(context->QueryInterface(IID_PPV_ARGS(&c))))return false;
    ComPtr<ID3D11Buffer> native;UINT first=0,count=0;
    c->PSGetConstantBuffers1(2,1,&native,&first,&count);
    D3D11_BUFFER_DESC desc{};if(native)native->GetDesc(&desc);
    if(!native || desc.ByteWidth<944 || desc.ByteWidth>65536 ||
       UINT64(first)*16+944>desc.ByteWidth || UINT64(count)*16<944)return false;
    if(!snapshot.buffer || snapshot.bytes!=desc.ByteWidth){
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        D3D11_BUFFER_DESC copy{};copy.ByteWidth=desc.ByteWidth;copy.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> buffer;
        if(FAILED(device->CreateBuffer(&copy,nullptr,&buffer)))return false;
        snapshot.buffer=buffer;snapshot.bytes=desc.ByteWidth;
    }
    context->CopyResource(snapshot.buffer.Get(),native.Get());
    snapshot.first=first;snapshot.count=count;return true;
}
inline void Bind(ID3D11DeviceContext* context,const Snapshot& snapshot) {
    ComPtr<ID3D11DeviceContext1> c;
    if(snapshot.buffer && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&c))))
        c->PSSetConstantBuffers1(1,1,snapshot.buffer.GetAddressOf(),&snapshot.first,&snapshot.count);
}
}
