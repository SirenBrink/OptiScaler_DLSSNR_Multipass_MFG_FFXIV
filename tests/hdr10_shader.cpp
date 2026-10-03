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
 struct K{unsigned w,h;float paper,peak,expand,contrast,saturation,vibrance;unsigned scene=0,reshadeHighlights=0,pad[2]{};float rect[4]{0,0,1,1};}k{260,1,203,1100,0,1,1,0};D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(k);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer>cb;check(d->CreateBuffer(&bd,nullptr,&cb));
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
 // Real upstream highlight detail: SDR pixels are identical, scene values differ.
 for(int x=0;x<260;x++)for(int k=0;k<3;k++)pixels[x*4+k]=1.f;
 for(int k=0;k<3;k++)pixels[64*4+k]=.25f; // opaque changed UI stays SDR
 c->UpdateSubresource(src.Get(),0,nullptr,pixels.data(),260*16,0);
 std::vector<float> scenePixels(260*4,2.f), referencePixels(260*4,1.f);
 for(int k=0;k<3;k++){scenePixels[4+k]=3;scenePixels[8+k]=4;}
 t.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;t.Usage=D3D11_USAGE_DEFAULT;t.CPUAccessFlags=0;t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 ComPtr<ID3D11Texture2D>scene,reference;data.pSysMem=scenePixels.data();check(d->CreateTexture2D(&t,&data,&scene));
 data.pSysMem=referencePixels.data();check(d->CreateTexture2D(&t,&data,&reference));
 ComPtr<ID3D11ShaderResourceView>sceneView,referenceView;check(d->CreateShaderResourceView(scene.Get(),nullptr,&sceneView));check(d->CreateShaderResourceView(reference.Get(),nullptr,&referenceView));
 ID3D11ShaderResourceView* guides[]={sceneView.Get(),referenceView.Get()};c->CSSetShaderResources(1,2,guides);
 k.scene=1;k.contrast=1;k.saturation=1;k.vibrance=0;k.expand=0;
 for(float peak:{400.f,1100.f,4000.f}){
 k.peak=peak;c->UpdateSubresource(cb.Get(),0,nullptr,&k,0,0);c->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);c->Dispatch(33,1,1);
 ID3D11UnorderedAccessView*nullU=nullptr;c->CSSetUnorderedAccessViews(0,1,&nullU,nullptr);c->CopyResource(read.Get(),dst.Get());
 D3D11_MAPPED_SUBRESOURCE m{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&m));auto*p=(unsigned*)m.pData;
 double a=decode(p[0]&1023),b=decode(p[1]&1023),cc=decode(p[2]&1023);
 expect(a>203 && b>a && cc>b && cc<=peak*1.01);
 double ui=decode(p[64]&1023),expected=203*pow((.25+.055)/1.055,2.4);expect(fabs(ui-expected)<expected*.03);
 printf("PASS: RenoDX scene peak %.0f: identical SDR whites preserve %.1f/%.1f/%.1f-nit scene detail; changed UI remains %.1f nits\n",peak,a,b,cc,ui);
 c->Unmap(read.Get(),0);
 }
 // Discard padding surrounding a smaller logical scene viewport, at both edges.
 std::fill(scenePixels.begin(),scenePixels.end(),0.f);
 for(int x=80;x<180;x++)for(int channel=0;channel<3;channel++)scenePixels[x*4+channel]=3.f;
 c->UpdateSubresource(scene.Get(),0,nullptr,scenePixels.data(),260*16,0);
 k.rect[0]=80.f/260;k.rect[1]=0;k.rect[2]=100.f/260;k.rect[3]=1;k.peak=1100;
 c->UpdateSubresource(cb.Get(),0,nullptr,&k,0,0);c->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);c->Dispatch(33,1,1);
 ID3D11UnorderedAccessView* nullU=nullptr;c->CSSetUnorderedAccessViews(0,1,&nullU,nullptr);c->CopyResource(read.Get(),dst.Get());
 D3D11_MAPPED_SUBRESOURCE mapped{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));auto* cropped=(unsigned*)mapped.pData;
 expect(decode(cropped[0]&1023)>800 && cropped[0]==cropped[259]);
 for(int x=0;x<260;x++)if(x!=64)expect(cropped[x]==cropped[0]);
 c->Unmap(read.Get(),0);puts("PASS: viewport cropping preserves highlights at both image edges and excludes black allocation padding");
 // ReShade changes highlight colour: strict rejection loses HDR brightness.
 // The opt-in restores measured energy with one gain, retaining the final hue.
 for(int x=0;x<260;x++)for(int channel=0;channel<3;channel++){
     pixels[x*4+channel]=channel==0?.95f:channel==1?.55f:.15f;
     scenePixels[x*4+channel]=channel==2?4.f:1.f;referencePixels[x*4+channel]=1.f;
 }
 for(int channel=0;channel<3;channel++)pixels[64*4+channel]=.25f;
 c->UpdateSubresource(src.Get(),0,nullptr,pixels.data(),260*16,0);
 c->UpdateSubresource(scene.Get(),0,nullptr,scenePixels.data(),260*16,0);
 c->UpdateSubresource(reference.Get(),0,nullptr,referencePixels.data(),260*16,0);
 k.rect[0]=k.rect[1]=0;k.rect[2]=k.rect[3]=1;unsigned baseline=0;
 for(unsigned option:{0u,1u}){
     k.reshadeHighlights=option;c->UpdateSubresource(cb.Get(),0,nullptr,&k,0,0);
     c->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);c->Dispatch(33,1,1);
     c->CSSetUnorderedAccessViews(0,1,&nullU,nullptr);c->CopyResource(read.Get(),dst.Get());
     check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));cropped=(unsigned*)mapped.pData;
     if(!option)baseline=cropped[0];
     else {
         expect(decode(cropped[0]&1023)>decode(baseline&1023)*2);
         double r20=decode(cropped[0]&1023),g20=decode((cropped[0]>>10)&1023),b20=decode((cropped[0]>>20)&1023);
         double r=1.660491*r20-.587641*g20-.072850*b20;
         double g=-.124550*r20+1.132900*g20-.008349*b20;
         double b=-.018151*r20-.100579*g20+1.118730*b20;
         auto linear=[](double v){return pow((v+.055)/1.055,2.4);};
         expect(fabs(r/g-linear(.95)/linear(.55))<.04*linear(.95)/linear(.55));
         expect(fabs(b/g-linear(.15)/linear(.55))<.015);
         printf("PASS: ReShade highlight opt-in restores %.1f -> %.1f-nit red-channel brightness while preserving final RGB ratios\n",decode(baseline&1023),r20);
     }
     double ui=decode(cropped[64]&1023);expect(fabs(ui-203*pow((.25+.055)/1.055,2.4))<.4);
     c->Unmap(read.Get(),0);
 }
 // With no extended scene energy, the option cannot invent HDR highlights.
 for(int channel=0;channel<3;channel++)scenePixels[channel]=1;
 c->UpdateSubresource(scene.Get(),0,nullptr,scenePixels.data(),260*16,0);
 c->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);c->Dispatch(33,1,1);
 c->CSSetUnorderedAccessViews(0,1,&nullU,nullptr);c->CopyResource(read.Get(),dst.Get());
 check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));expect(((unsigned*)mapped.pData)[0]==baseline);c->Unmap(read.Get(),0);
 return 0;
}catch(const std::exception&e){puts(e.what());return 1;}
