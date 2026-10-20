#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <cstdio>
#include <cstring>
#include <cstdint>
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_FUNC() ((void)0)
constexpr int BUFFER_COUNT=4;
bool ok=true;
#define CHECK(x) do{if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);ok=false;}}while(0)
template<class T>struct Setting{T value{};T value_or_default()const{return value;}void reset(){value={};}};
struct Config{Setting<bool>FGEnabled{true},FGXeFGIgnoreInitChecks{false},FGXeFGDebugView{false};static Config*Instance(){static Config x;return &x;}};
enum class FGInput{Upscaler,Other};
enum GameQuirk{ForceFGRenderSizeMVs=1};
struct Feature{unsigned w=2560,out=3840;unsigned RenderWidth(){return w;}unsigned DisplayWidth(){return out;}};
struct State{bool isShuttingDown=false,fgChanged=false,scChanged=false,clearCapturedHudlesses=false,realExclusiveFullscreen=false;void*currentFGSwapchain=nullptr;FGInput activeFgInput=FGInput::Upscaler;Feature*currentFeature=nullptr;unsigned gameQuirks=0;static State&Instance(){static State s;return s;}};
struct OwnedMutex{int owner=0;int getOwner(){return owner;}void unlockThis(int){owner=0;}};
struct OwnedLockGuard{OwnedLockGuard(OwnedMutex&,int){}};
struct ID3D12Device{};
struct ID3D12CommandList{HRESULT Close(){return S_OK;}};
struct Queue{HRESULT Signal(void*,UINT64){return S_OK;}};
namespace Hdr10{void ExecuteCommands(Queue*,unsigned,ID3D12CommandList**) {}}
namespace Hudfix_Dx12{void ResetCounters(){}}
namespace magic_enum{template<class T>const char*enum_name(T){return "mock";}}
namespace sl{enum class Result{eOk};enum class DLSSGMode{eOff};enum class DLSSGQueueParallelismMode{eBlockPresentingClientQueue};enum class ReflexMode{eOff};struct DLSSGOptions{DLSSGMode mode{};DLSSGQueueParallelismMode queueParallelismMode{};};struct ReflexOptions{ReflexMode mode{};bool useMarkersToOptimize=false;};}
bool available=true;unsigned enabledCalls=0,disabledCalls=0,enableFailures=0,objectCreations=0;
sl::Result Options(int,const sl::DLSSGOptions&){++disabledCalls;return sl::Result::eOk;}
sl::Result Reflex(const sl::ReflexOptions&){return sl::Result::eOk;}
struct StreamlineProxy{static bool LoadStreamline(){return available;}static auto DLSSGSetOptions(){return Options;}static auto ReflexSetOptions(){return Reflex;}};
enum xefg_swapchain_result_t{XEFG_SWAPCHAIN_RESULT_SUCCESS,MOCK_FAILURE};
xefg_swapchain_result_t SetEnabled(void*,bool enabled){if(!enabled){++disabledCalls;return XEFG_SWAPCHAIN_RESULT_SUCCESS;}++enabledCalls;if(enableFailures){--enableFailures;return MOCK_FAILURE;}return XEFG_SWAPCHAIN_RESULT_SUCCESS;}
struct XeFGProxy{static bool InitXeFG(){return available;}static auto SetEnabled(){return ::SetEnabled;}};
namespace XeFGPacing{void RequestRecovery(){}}
enum class FG_Flags{DisplayResolutionMVs,InfiniteDepth};
struct Flags{unsigned value=0;bool operator[](FG_Flags f)const{return value&(1u<<unsigned(f));}bool operator&(FG_Flags f)const{return (*this)[f];}};
struct FG_Constants{Flags flags{};unsigned displayWidth=3840,displayHeight=2160;};
struct History{void Invalidate(){}};
struct Guide{bool valid=false;};
class IFGFeature{
public:
 UINT64 _frameCount=0,_targetFrame=0,_lastDispatchedFrame=0,_lastFGFrame=0;
 bool _isActive=false,_waitingNewFrameData=false;
 bool _waitingExecute[BUFFER_COUNT]{},_noUi[BUFFER_COUNT]{},_noDistortionField[BUFFER_COUNT]{},_noHudless[BUFFER_COUNT]{};
 std::map<int,bool>_resourceReady[BUFFER_COUNT];std::map<int,UINT64>_resourceFrame;
 FG_Constants _constants{};OwnedMutex Mutex;
 virtual ~IFGFeature()=default;virtual void Activate()=0;virtual void Deactivate()=0;virtual void NewFrame()=0;
 const char*Name(){return "test";}
 int GetIndex();UINT64 StartNewFrame();bool IsActive();bool IsPaused();bool IsLowResMV();void SetFrameCount(UINT64);void UpdateTarget();void ResetCounters();
};
class IFGFeature_Dx12:public IFGFeature{
public:
 ID3D12Device*_device=nullptr;Queue*_gameCommandQueue=nullptr;void*_swapChain=nullptr;void*_uiFence=nullptr;
 bool _uiCommandListResetted[BUFFER_COUNT]{};ID3D12CommandList*_uiCommandList[BUFFER_COUNT]{};UINT64 _uiAllocatorFenceValues[BUFFER_COUNT]{};
 std::map<int,int>_frameResources[BUFFER_COUNT];std::shared_mutex _resourceMutex[BUFFER_COUNT];
 UINT64 _lastFGFramePresentId=0,_fgFramePresentId=0;
 History _sceneHistory;std::array<std::array<Guide,2>,BUFFER_COUNT>_guideSnapshots{};
 bool SubmitUICommandList(unsigned){return true;}
 void CreateObjects(ID3D12Device*d){CHECK(d);++objectCreations;_device=d;}
 void CreateContext(ID3D12Device*,FG_Constants&);
 void NewFrame();
};
class DLSSG_Dx12:public IFGFeature_Dx12{
public:
 std::atomic<bool>_ownsFramePacing{false};std::atomic<UINT64>_observedMultiplier{0};History _historyFrames;int _sampleMode=-1,viewport=0;
 void CreateContext(ID3D12Device*,FG_Constants&);void Activate();void Deactivate();void EvaluateState(ID3D12Device*,FG_Constants&);bool DispatchGate();
};
class XeFG_Dx12:public IFGFeature_Dx12{
public:
 void*_swapChainContext=nullptr,*_fgContext=nullptr;bool _infiniteDepth=false;
 void CreateContext(ID3D12Device*,FG_Constants&);void DestroyFGContext(){Deactivate();_fgContext=nullptr;}
 void Activate();void Deactivate();void EvaluateState(ID3D12Device*,FG_Constants&);bool DispatchGate();
};
class Sl_Inputs_Dx12{public:
 std::mutex _frameBoundaryMutex;bool _isFrameFinished=false;uint32_t _lastPresentFrameId=0,_currentFrameId=0;int _currentIndex=-1;uint32_t _frameIdIndex[BUFFER_COUNT]{};
 void CheckForFrame(IFGFeature_Dx12*,uint32_t);
};
#include "fg-startup-production.inl"
void Reset(){State::Instance()={};Config::Instance()->FGEnabled.value=true;Config::Instance()->FGXeFGIgnoreInitChecks.value=false;available=true;enabledCalls=disabledCalls=enableFailures=objectCreations=0;}
ID3D12Device device;Queue queue;int swapchain;
template<class T>void Ready(T&fg){fg._gameCommandQueue=&queue;fg._swapChain=&swapchain;State::Instance().currentFGSwapchain=&swapchain;if constexpr(std::is_same_v<T,XeFG_Dx12>)fg._swapChainContext=&swapchain;}
template<class T>void Tick(T&fg,FG_Constants&c,bool newFrameFirst=false){if(newFrameFirst)fg.StartNewFrame();fg.EvaluateState(&device,c);if(!newFrameFirst)fg.StartNewFrame();}
template<class T>void Rewind(){
 Reset();T fg;fg._frameCount=1000;fg.UpdateTarget();fg._lastDispatchedFrame=999;fg._resourceReady[0][0]=true;fg.SetFrameCount(999);CHECK(fg._targetFrame==1010);CHECK(fg._resourceReady[0].contains(0));fg.SetFrameCount(1000);fg.SetFrameCount(0);
 for(int i=0;i<12;++i)fg.StartNewFrame();CHECK(!fg.IsPaused());CHECK(fg._lastDispatchedFrame<=fg._frameCount);CHECK(fg._resourceReady[0].empty());
}
template<class T>void Matrix(){
 for(bool first:{false,true})for(bool initiallyEnabled:{false,true}){
  Reset();T fg;FG_Constants c;Config::Instance()->FGEnabled.value=initiallyEnabled;
  fg.Activate();CHECK(!fg._isActive);CHECK(enabledCalls==0); // no device/context/swapchain yet
  fg.EvaluateState(nullptr,c);CHECK(objectCreations==0);
  Ready(fg);available=false;for(int i=0;i<3;++i)Tick(fg,c,first);CHECK(!fg._isActive);available=true;
  for(int i=0;i<24;++i)Tick(fg,c,first);CHECK(fg._isActive==initiallyEnabled);
  if(!initiallyEnabled){Config::Instance()->FGEnabled.value=true;State::Instance().fgChanged=true;for(int i=0;i<24;++i)Tick(fg,c,first);}
  CHECK(fg._isActive);CHECK(fg.DispatchGate());
  State::Instance().fgChanged=true;Tick(fg,c,first);CHECK(!fg._isActive);for(int i=0;i<24;++i)Tick(fg,c,first);CHECK(fg._isActive); // load/settings recovery
  fg.Deactivate();fg._waitingNewFrameData=true;CHECK(!fg.DispatchGate());Tick(fg,c,first);for(int i=0;i<24;++i)Tick(fg,c,first);CHECK(fg._isActive);CHECK(!fg._waitingNewFrameData);
  fg.Deactivate();fg._waitingNewFrameData=true;Config::Instance()->FGEnabled.value=false;fg._targetFrame=0;fg.StartNewFrame();CHECK(!fg._isActive);CHECK(!fg._waitingNewFrameData);CHECK(fg._targetFrame==0);
  fg._waitingNewFrameData=true;fg.Deactivate();CHECK(!fg._waitingNewFrameData);
  Config::Instance()->FGEnabled.value=true;State::Instance().fgChanged=true;for(int i=0;i<24;++i)Tick(fg,c,first);CHECK(fg._isActive);
  Config::Instance()->FGEnabled.value=false;fg.EvaluateState(nullptr,c);CHECK(!fg._isActive); // off must work even with a missing input device
  State::Instance().isShuttingDown=true;Config::Instance()->FGEnabled.value=true;fg._waitingNewFrameData=true;fg.StartNewFrame();CHECK(!fg._isActive);CHECK(!fg._waitingNewFrameData);fg.Activate();CHECK(!fg._isActive);
 }
 Rewind<T>();
}
int main(int argc,char**argv){
 if(argc>1){
  if(!strcmp(argv[1],"rewind-only")){Rewind<DLSSG_Dx12>();Rewind<XeFG_Dx12>();}
  else if(!strcmp(argv[1],"disabled-recovery-only")){Reset();DLSSG_Dx12 fg;Ready(fg);fg._device=&device;Config::Instance()->FGEnabled.value=false;fg._waitingNewFrameData=true;fg.StartNewFrame();CHECK(fg._targetFrame==0);CHECK(!fg._waitingNewFrameData);CHECK(!fg._isActive);}
  else if(!strcmp(argv[1],"pending-dispatch-only")){Reset();DLSSG_Dx12 a;XeFG_Dx12 b;a._waitingNewFrameData=b._waitingNewFrameData=true;CHECK(!a.DispatchGate());CHECK(!b.DispatchGate());}
  else if(!strcmp(argv[1],"ui-preference-only")){Reset();ApplyXeFGMenuWarnings(false,true);CHECK(Config::Instance()->FGEnabled.value);}
  else if(!strcmp(argv[1],"input-epoch-only")){Reset();DLSSG_Dx12 fg;Sl_Inputs_Dx12 input;input._currentFrameId=1000;input._lastPresentFrameId=0;input._isFrameFinished=true;input.CheckForFrame(&fg,1);CHECK(fg._frameCount==1);CHECK(input._currentFrameId==1);}
  else {CHECK(false);}
  return ok?0:1;
 }
 Matrix<DLSSG_Dx12>();Matrix<XeFG_Dx12>();
 for(uint32_t next:{0u,1u}){Reset();DLSSG_Dx12 fg;Sl_Inputs_Dx12 input;input._currentFrameId=1000;input._lastPresentFrameId=0;input._isFrameFinished=true;input.CheckForFrame(&fg,next);CHECK(fg._frameCount==1);CHECK(input._currentFrameId==1);auto count=fg._frameCount;input.CheckForFrame(&fg,1);CHECK(fg._frameCount==count);}
 {Reset();DLSSG_Dx12 fg;fg._frameCount=1000;Sl_Inputs_Dx12 input;input._currentFrameId=input._lastPresentFrameId=1000;input._isFrameFinished=true;input.CheckForFrame(&fg,0);CHECK(input._currentFrameId==1001);CHECK(fg._frameCount==1001);} // zero means unknown during an ordinary frame

 for(bool vectors:{false,true})for(bool exclusive:{false,true}){Reset();ApplyXeFGMenuWarnings(vectors,exclusive);CHECK(Config::Instance()->FGEnabled.value);}
 Reset();XeFG_Dx12 fg;FG_Constants c;Ready(fg);enableFailures=1;for(int i=0;i<24;++i)Tick(fg,c);CHECK(fg._isActive);CHECK(enabledCalls>=2);
 Reset();XeFG_Dx12 high;Ready(high);high._device=&device;high._fgContext=&swapchain;high._constants.flags.value=1;high.Activate();CHECK(!high._isActive); // unsupported dilated vectors still require explicit override
 Config::Instance()->FGXeFGIgnoreInitChecks.value=true;high.Activate();CHECK(high._isActive);
 if(ok)puts("PASS production DLSS-G/XeFG lifecycle: saved on/off, delayed SDK/device, enable retry, loading, pending recovery, disabled/shutdown, dispatch gate and input frame-ID restart");
 return ok?0:1;
}
