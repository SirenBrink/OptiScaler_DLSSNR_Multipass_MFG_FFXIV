#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using UINT=unsigned;using UINT64=uint64_t;
#define BUFFER_COUNT 4
#define LOG_INFO(...) ((void)0)
enum { D3D12_RESOURCE_STATE_COPY_DEST=1,D3D12_RESOURCE_STATE_COPY_SOURCE=2,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=3 };
struct D3D12_RESOURCE_DESC {int Dimension=2,Width=4,Height=2,Format=1,DepthOrArraySize=1,MipLevels=1;struct {int Count=1,Quality=0;} SampleDesc;int Flags=0;};
struct ID3D12Resource {D3D12_RESOURCE_DESC desc; int value=0,state=3,released=0;D3D12_RESOURCE_DESC GetDesc(){return desc;} void Release(){++released;}};
struct ID3D12Fence {uint64_t completed=0;int refs=1;void AddRef(){++refs;}void Release(){--refs;}uint64_t GetCompletedValue(){return completed;}};
struct ID3D12GraphicsCommandList {};
enum class FG_ResourceType {Velocity,Depth};enum class FG_ResourceValidity {ValidNow,Other};
struct Dx12Resource {FG_ResourceType type;ID3D12Resource* resource=nullptr;UINT top=0,left=0;UINT64 width=4;UINT height=2;ID3D12GraphicsCommandList* cmdList=nullptr;int state=3;FG_ResourceValidity validity=FG_ResourceValidity::ValidNow;ID3D12Resource* copy=nullptr;int frameIndex=-1;};
struct DLSSG_Dx12 {
#include "guide-members.inl"
    UINT64 _frameCount=0,_uiFenceValue=0;ID3D12Fence* _uiFence=nullptr;bool _uiCommandListResetted[4]{};UINT64 _uiAllocatorFenceValues[4]{};ID3D12GraphicsCommandList* _uiCommandList[4]{};
    float _jitterX[4]{},_jitterY[4]{},_mvScaleX[4]{},_mvScaleY[4]{},_cameraNear[4]{},_cameraFar[4]{},_cameraVFov[4]{},_cameraAspectRatio[4]{},_meterFactor[4]{};
    float _cameraPosition[4][3]{},_cameraUp[4][3]{},_cameraRight[4][3]{},_cameraForward[4][3]{};double _ftDelta[4]{};UINT _reset[4]{};
    int GetIndex(){return int(_frameCount%4);}
    void SetPresentationGuideDelay(int age);
    void SetJitter(float a,float b,int i){_jitterX[i]=a;_jitterY[i]=b;}
    void SetMVScale(float a,float b,int i){_mvScaleX[i]=a;_mvScaleY[i]=b;}
    void SetCameraValues(float a,float b,float c,float d,float e,int i){_cameraNear[i]=a;_cameraFar[i]=b;_cameraVFov[i]=c;_cameraAspectRatio[i]=d;_meterFactor[i]=e;}
    void SetFrameTimeDelta(double a,int i){_ftDelta[i]=a;}void SetReset(UINT a,int i){_reset[i]=a;}
    void ResourceBarrier(ID3D12GraphicsCommandList*,ID3D12Resource* r,int a,int b){assert(r->state==a);r->state=b;}
    bool CopyResource(ID3D12GraphicsCommandList* c,ID3D12Resource* s,ID3D12Resource** d,int state){
      ResourceBarrier(c,s,state,2);if(!*d){*d=new ID3D12Resource;(*d)->desc=s->desc;(*d)->state=1;}
      assert((*d)->state==1);(*d)->value=s->value;ResourceBarrier(c,s,2,state);return true;}
};
#include "guide-methods.inl"
int main(){
 DLSSG_Dx12 g;ID3D12GraphicsCommandList cmd[4];ID3D12Fence fence;g._uiFence=&fence;
 for(int i=0;i<4;++i)g._uiCommandList[i]=&cmd[i];
 ID3D12Resource mv,depth;
 auto frame=[&](unsigned n,int age,bool reset=false){
   g._frameCount=n;g._uiFenceValue=n+1;int i=n%4;g._reset[i]=reset;g._jitterX[i]=float(n);g._cameraNear[i]=float(n+100);
   g.SetPresentationGuideDelay(age);mv.value=int(n);depth.value=int(n+1000);
   Dx12Resource v{FG_ResourceType::Velocity,&mv},d{FG_ResourceType::Depth,&depth};v.cmdList=d.cmdList=&cmd[i];v.width=d.width=mv.desc.Width;
   bool a=g.MatchPresentationGuide(v,i),b=g.MatchPresentationGuide(d,i);assert(a==b);
   if(a){assert(v.resource->value==int(n-age));assert(d.resource->value==int(n-age+1000));assert(g._jitterX[i]==float(n-age));assert(g._cameraNear[i]==float(n-age+100));assert(v.frameIndex==i&&v.cmdList==&cmd[i]);}
   assert(mv.state==3&&depth.state==3);return a;
 };
 assert(!frame(10,2));assert(!frame(11,2));for(unsigned i=12;i<30;++i)assert(frame(i,2));
 assert(!frame(34,2));assert(!frame(35,2));assert(frame(36,2)); // gap
 assert(!frame(37,2,true));assert(!frame(38,2));assert(frame(39,2)); // reset
 mv.desc.Width=depth.desc.Width=8;
 assert(!frame(40,2));assert(!frame(41,2));assert(frame(42,2)); // resize
 g.SetPresentationGuideDelay(-1);assert(!frame(43,2));assert(!frame(44,2));assert(frame(45,2));
 g.SetPresentationGuideDelay(-1);assert(frame(46,0));assert(frame(47,1));assert(frame(48,2)); // warm-up repeats first scene
 ID3D12Resource kept;ID3D12Resource* p=&kept;g._uiFenceValue=99;g.RetireGuide(p);assert(!p);
 fence.completed=99;g._uiCommandListResetted[0]=true;g._uiAllocatorFenceValues[0]=99;g.CollectGuides();assert(kept.released==0);
 g._uiCommandListResetted[0]=false;g.CollectGuides();assert(kept.released==1);
 puts("Production guide history: pixels/metadata, warm-up, wrap, gaps, reset, resize, toggle, and pending-list retirement passed");
}
