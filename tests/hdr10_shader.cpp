#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include "../OptiScaler/shaders/hdr/Hdr10_Shader.h"
#pragma comment(lib,"d3d11.lib")
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D11 failure");}
void expect(bool b){if(!b)throw std::runtime_error("HDR ramp validation failed");}
double decode(unsigned v){double p=pow(v/1023.,32./2523.);return 10000*pow(fmax(p-3424./4096.,0)/(2413./128.-2392./128.*p),16384./2610.);}
int main()try{
 ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>c;check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
 ComPtr<ID3D11ComputeShader>shader;check(d->CreateComputeShader(Hdr10_cso,sizeof(Hdr10_cso),nullptr,&shader));
 std::vector<float>pixels(260*4,1);for(int x=0;x<256;x++)for(int k=0;k<3;k++)pixels[x*4+k]=x/255.f;
 for(int x=256;x<259;x++)for(int k=0;k<3;k++)pixels[x*4+k]=k==x-256?1.f:0.f;
 pixels[259*4]=.5f;pixels[259*4+1]=.4f;pixels[259*4+2]=.3f;
 D3D11_TEXTURE2D_DESC t{};t.Width=260;t.Height=t.MipLevels=t.ArraySize=t.SampleDesc.Count=1;t.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 D3D11_SUBRESOURCE_DATA data{pixels.data(),260*16,0};ComPtr<ID3D11Texture2D>src,dst,read;check(d->CreateTexture2D(&t,&data,&src));ComPtr<ID3D11ShaderResourceView>srv;check(d->CreateShaderResourceView(src.Get(),nullptr,&srv));
 t.Format=DXGI_FORMAT_R10G10B10A2_UNORM;t.BindFlags=D3D11_BIND_UNORDERED_ACCESS;check(d->CreateTexture2D(&t,nullptr,&dst));ComPtr<ID3D11UnorderedAccessView>uav;check(d->CreateUnorderedAccessView(dst.Get(),nullptr,&uav));
 t.BindFlags=0;t.Usage=D3D11_USAGE_STAGING;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(d->CreateTexture2D(&t,nullptr,&read));
 struct K{unsigned w,h;float paper,peak,expand,contrast,saturation,vibrance;}k{260,1,203,1100,0,1,1,0};D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(k);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer>cb;check(d->CreateBuffer(&bd,nullptr,&cb));
 for(float contrast:{.5f,1.f,1.5f}) {
 k.contrast=contrast;
 for(float expansion:{0.f,.5f,1.f}){
 k.expand=expansion;c->UpdateSubresource(cb.Get(),0,nullptr,&k,0,0);c->CSSetShader(shader.Get(),nullptr,0);c->CSSetShaderResources(0,1,srv.GetAddressOf());c->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);c->CSSetConstantBuffers(0,1,cb.GetAddressOf());c->Dispatch(33,1,1);
 ID3D11UnorderedAccessView*nullU=nullptr;c->CSSetUnorderedAccessViews(0,1,&nullU,nullptr);c->CopyResource(read.Get(),dst.Get());D3D11_MAPPED_SUBRESOURCE m{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&m));auto*p=(unsigned*)m.pData;
 expect((p[0]&0x3fffffff)==0);double previous=-1;
 for(int x=0;x<256;x++){auto rr=p[x]&1023,gg=(p[x]>>10)&1023,bb=(p[x]>>20)&1023;expect(abs(int(rr)-int(gg))<=1&&abs(int(rr)-int(bb))<=1);double n=decode(rr);expect(n>=previous&&n<=1110);previous=n;}
 double target=203+897*expansion;expect(fabs(previous-target)<target*.01);expect((p[256]&1023)>((p[256]>>10)&1023));expect(((p[257]>>10)&1023)>(p[257]&1023));expect(((p[258]>>20)&1023)>(p[258]&1023));
 // Shadows must remain stable; a 70% sRGB highlight must now benefit from
 // expansion (the original 0.5-linear knee left it unchanged).
 double shadow=decode(p[64]&1023), highlight=decode(p[179]&1023);
 double shadowSdr=203*pow((64/255.+.055)/1.055,2.4);
 double highlightSdr=203*pow((179/255.+.055)/1.055,2.4);
 if(contrast==1) expect(fabs(shadow-shadowSdr)<shadowSdr*.02);
 if(contrast>1) expect(shadow<shadowSdr);
 if(contrast<1) expect(shadow>shadowSdr);
 if(expansion>0 && contrast>=1) expect(highlight>highlightSdr*1.4);
 printf("PASS: contrast %.1f, expansion %.1f, black 0, white %.2f nits, highlight %.2f nits, monotonic ramp\n",contrast,expansion,previous,highlight);c->Unmap(read.Get(),0);
 }
 }
 // Compare the actual compiled shader at neutral/desaturated/boosted settings.
 // Preserve neutral pixels and luminance, including at the gamut boundary.
 double baseChroma=0, baseLuma=0; unsigned neutral[256]{};
 const float colours[][2]={{1,0},{0,0},{2,0},{1,1},{1,-1},{2,1}};
 for(int setting=0;setting<6;setting++) {
 k.contrast=1;k.expand=.5f;k.saturation=colours[setting][0];k.vibrance=colours[setting][1];
 c->UpdateSubresource(cb.Get(),0,nullptr,&k,0,0);c->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);c->Dispatch(33,1,1);
 ID3D11UnorderedAccessView*nullU=nullptr;c->CSSetUnorderedAccessViews(0,1,&nullU,nullptr);c->CopyResource(read.Get(),dst.Get());
 D3D11_MAPPED_SUBRESOURCE m{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&m));auto*p=(unsigned*)m.pData;
 for(int x=0;x<256;x++) { if(setting==0) neutral[x]=p[x]; else expect(p[x]==neutral[x]); }
 for(int x=0;x<260;x++) for(int channel=0;channel<3;channel++) expect(decode((p[x]>>(10*channel))&1023)<=1110);
 double r=decode(p[259]&1023),g=decode((p[259]>>10)&1023),b=decode((p[259]>>20)&1023);
 double luma=.2627*r+.6780*g+.0593*b, chroma=fmax(r,fmax(g,b))-fmin(r,fmin(g,b));
 if(setting==0) {baseChroma=chroma;baseLuma=luma;}
 else expect(fabs(luma-baseLuma)<baseLuma*.02);
 if(k.saturation==0) {
   expect(chroma<.001);
   for(int x=256;x<260;x++) expect((p[x]&1023)==((p[x]>>10)&1023) && (p[x]&1023)==((p[x]>>20)&1023));
 }
 if(setting==2 || setting==3 || setting==5) expect(chroma>baseChroma);
 if(setting==4) expect(chroma<baseChroma);
 printf("PASS: saturation %.1f, vibrance %.1f: neutral pixels unchanged, bounded output, luminance preserved\n",k.saturation,k.vibrance);
 c->Unmap(read.Get(),0);
 }
 return 0;
}catch(const std::exception&e){puts(e.what());return 1;}
