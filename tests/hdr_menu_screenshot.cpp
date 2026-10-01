#define NOMINMAX
#include "../OptiScaler/shaders/hdr/HdrScreenshotImage.h"
#include "../OptiScaler/shaders/hdr/HdrMenu.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <source_location>
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
using Hdr10::ScreenshotImage::Check;
void expect(bool ok, std::source_location where=std::source_location::current()) {
    if (!ok) throw std::runtime_error("HDR validation failed at line "+std::to_string(where.line()));
}
float pq(float n) { double p=pow(n/10000.,2610./16384.); return float(pow((3424./4096.+2413./128.*p)/(1+2392./128.*p),2523./32.)); }
uint32_t pack(float r,float g,float b) { return uint32_t(lround(pq(r)*1023)) | (uint32_t(lround(pq(g)*1023))<<10) | (uint32_t(lround(pq(b)*1023))<<20) | 0xc0000000; }
int main(int argc,char** argv) try {
    setvbuf(stdout,nullptr,_IONBF,0);
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    struct ComScope { ~ComScope() { CoUninitialize(); } } comScope;
    // Use a padded row stride and distinctly different second row to catch
    // orientation/footprint errors as well as accidental SDR clipping.
    std::array<uint32_t,128> src{};
    src[0]=pack(0,0,0); src[1]=pack(203,203,203); src[2]=pack(1100,1100,1100);
    src[64]=pack(1000,0,0); src[65]=pack(0,1000,0); src[66]=pack(0,0,1000);
    auto path=std::filesystem::path(argc>1?argv[1]:"hdr-roundtrip.png");
    Hdr10::ScreenshotImage::SavePng(path,src.data(),3,2,256,true);
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapDecoder> decoder; Check(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder));
    ComPtr<IWICBitmapFrameDecode> frame; Check(decoder->GetFrame(0,&frame));
    WICPixelFormatGUID format; Check(frame->GetPixelFormat(&format)); expect(format==GUID_WICPixelFormat48bppRGB);
    UINT w,h;Check(frame->GetSize(&w,&h));expect(w==3&&h==2);
    std::array<uint16_t,18> pixels{};Check(frame->CopyPixels(nullptr,18,36,reinterpret_cast<BYTE*>(pixels.data())));
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x) {
        for(unsigned k=0;k<3;++k) {
            unsigned original=(src[y*64+x]>>(10*k))&1023;
            unsigned recovered=unsigned(lround(pixels[(y*3+x)*3+k]*1023.0/65535));
            expect(original==recovered);
        }
    }
    expect(fabs(Hdr10::ScreenshotImage::DecodePQ(pixels[3]/65535.f)-203)<2);
    expect(fabs(Hdr10::ScreenshotImage::DecodePQ(pixels[6]/65535.f)-1100)<10);
    // Verify complete PNG chunks, correct colour labels, CRCs and no SDR tags
    // on the PQ image. The known CRC vector checks the checksum independently.
    expect(Hdr10::ScreenshotImage::Crc(reinterpret_cast<const BYTE*>("123456789"),9)==0xcbf43926);
    auto metadata=[](const std::filesystem::path& file,bool hdr) {
        std::ifstream in(file,std::ios::binary);std::vector<BYTE> b((std::istreambuf_iterator<char>(in)),{});
        unsigned colour=0,idat=0;bool end=false;
        expect(b.size()>33);
        expect(b[24]==(hdr?16:8) && b[25]==2); // RGB, 16-bit HDR / 8-bit sharing.
        for(size_t i=8;i<b.size();) {
            expect(b.size()-i>=12);size_t n=Hdr10::ScreenshotImage::ReadBE(&b[i]);expect(n<=b.size()-i-12);
            auto*p=&b[i+4];expect(Hdr10::ScreenshotImage::ReadBE(p+4+n)==Hdr10::ScreenshotImage::Crc(p,n+4));
            if(!memcmp(p,"IDAT",4))++idat;
            if(!memcmp(p,"cICP",4)) {expect(hdr && !idat && n==4 && p[4]==9 && p[5]==16 && p[6]==0 && p[7]==1);++colour;}
            if(!memcmp(p,"sRGB",4)) {expect(!hdr && !idat && n==1);++colour;}
            expect(memcmp(p,"iCCP",4)&&memcmp(p,"gAMA",4)&&memcmp(p,"cHRM",4));
            i+=n+12;if(!memcmp(p,"IEND",4)){expect(i==b.size());end=true;}
        }
        expect(colour==1 && idat>0 && end);
    };
    metadata(path,true);
    puts("PASS: HDR PNG roundtrip preserves every 10-bit source value, nits, row stride; valid PQ/BT.2020 metadata and CRCs");
    const std::array<std::array<BYTE,3>,6> expectedSdr={{{0,0,0},{128,96,64},{255,255,255},{230,12,44},{11,199,53},{17,28,210}}};
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_TYPELESS,
        DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_TYPELESS};
    for(unsigned variant=0;variant<6;++variant) {
        std::array<BYTE,512> original{};
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x) {
            auto* p=original.data()+256*y+4*x;const auto& rgb=expectedSdr[y*3+x];
            p[0]=rgb[variant<3?0:2];p[1]=rgb[1];p[2]=rgb[variant<3?2:0];p[3]=BYTE(31+x);
        }
        auto sdrPath=path.parent_path()/(path.stem().string()+"-SDR-"+std::to_string(variant)+".png");
        Hdr10::ScreenshotImage::SavePng(sdrPath,original.data(),3,2,256,false,formats[variant]);
        metadata(sdrPath,false);
        ComPtr<IWICBitmapDecoder> sdrDecoder; Check(factory->CreateDecoderFromFilename(sdrPath.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&sdrDecoder));
        ComPtr<IWICBitmapFrameDecode>sdrFrame;Check(sdrDecoder->GetFrame(0,&sdrFrame));
        ComPtr<IWICFormatConverter> converter;Check(factory->CreateFormatConverter(&converter));
        Check(converter->Initialize(sdrFrame.Get(),GUID_WICPixelFormat24bppRGB,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
        std::array<BYTE,18>sdrPixels{};Check(converter->CopyPixels(nullptr,9,18,sdrPixels.data()));
        expect(memcmp(sdrPixels.data(),expectedSdr.data(),18)==0);
    }
    puts("PASS: SDR PNG retains every source byte for RGBA/BGRA, UNORM/sRGB/typeless, padded rows; no extra tone mapping");
    ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>c;
    Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
    const char* vs=R"(cbuffer Colour:register(b0){float4 colour;} struct O {float4 p:SV_POSITION;float4 c:COLOR0;float2 u:TEXCOORD0;};
    O main(uint i:SV_VertexID){O o;float2 v=float2((i<<1)&2,i&2);o.p=float4(v*float2(2,-2)+float2(-1,1),0,1);o.c=colour;o.u=.5;return o;})";
    ComPtr<ID3DBlob>vb,pb,errors;
    Check(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vb,&errors));
    Check(D3DCompile(Hdr10::MenuPixelShader,strlen(Hdr10::MenuPixelShader),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&pb,&errors));
    ComPtr<ID3D11VertexShader>vertex;ComPtr<ID3D11PixelShader>pixel;
    Check(d->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vertex));
    Check(d->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&pixel));
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    float white[]={1,1,1,1};D3D11_SUBRESOURCE_DATA data{white,16,0};ComPtr<ID3D11Texture2D>tex,target,read;
    Check(d->CreateTexture2D(&td,&data,&tex));ComPtr<ID3D11ShaderResourceView>srv;Check(d->CreateShaderResourceView(tex.Get(),nullptr,&srv));
    td.BindFlags=D3D11_BIND_RENDER_TARGET;Check(d->CreateTexture2D(&td,nullptr,&target));
    ComPtr<ID3D11RenderTargetView>rtv;Check(d->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(d->CreateTexture2D(&td,nullptr,&read));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer>colour,settings;
    Check(d->CreateBuffer(&bd,nullptr,&colour));Check(d->CreateBuffer(&bd,nullptr,&settings));
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState>sampler;Check(d->CreateSamplerState(&sd,&sampler));
    D3D11_VIEWPORT vp{0,0,1,1,0,1};c->RSSetViewports(1,&vp);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vertex.Get(),nullptr,0);c->PSSetShader(pixel.Get(),nullptr,0);
    c->VSSetConstantBuffers(0,1,colour.GetAddressOf());c->PSSetConstantBuffers(1,1,settings.GetAddressOf());
    c->PSSetShaderResources(0,1,srv.GetAddressOf());c->PSSetSamplers(0,1,sampler.GetAddressOf());c->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
    for(float mode:{0.f,1.f})for(float value:{0.f,.05f,.1f,.2f,.5f,1.f}) {
        float col[]={value,value,value,.4f},k[]={mode,0,0,0};
        c->UpdateSubresource(colour.Get(),0,nullptr,col,0,0);c->UpdateSubresource(settings.Get(),0,nullptr,k,0,0);c->Draw(3,0);c->CopyResource(read.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};Check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&m));auto*p=static_cast<float*>(m.pData);
        expect(fabs(p[3]-.4f)<.0001f);
        for(int i=0;i<3;++i) {
            if(mode==0)expect(fabs(p[i]-value)<.0001f);
            else { const float linear=value<=.04045f?value/12.92f:powf((value+.055f)/1.055f,2.4f); expect(fabs(Hdr10::ScreenshotImage::DecodePQ(p[i])-203.f*linear)<.15f); }
        }
        c->Unmap(read.Get(),0);
    }
    puts("PASS: actual menu shader on WARP: SDR unchanged, automatic HDR reference white, dark theme contrast and alpha preserved");
    return 0;
}catch(HRESULT hr){printf("HRESULT %08lx\n",hr);return 1;}catch(const std::exception&e){puts(e.what());return 1;}
