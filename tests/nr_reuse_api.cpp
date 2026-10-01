#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <nvapi.h>
#include <vector>
#include <map>
#include <tuple>
#include <mutex>
#include <stdexcept>
#include <cstdio>
#include <thread>
#include <future>
#include <chrono>
#include "../OptiScaler/dlssnr/DlssNrVitReuse.h"
#define LOG_INFO(...) ((void)0)
struct ObservedFunction { NVDX_ObjectHandle module=nullptr;std::string name; };
struct EvaluationObservation { uint64_t launches=0,known=0,unknown=0;bool reset=false;unsigned trace=0; };
EvaluationObservation& Observation(){static thread_local EvaluationObservation value;return value;}
struct Session { bool pending=false; };
struct State {
    DlssNrVitReuse::Filter vit;
    DlssNrVitReuse::RoleRegistry<NVDX_ObjectHandle> vitRole;
    std::map<NVDX_ObjectHandle,ObservedFunction>observedFunctions;
    std::map<std::tuple<ID3D12GraphicsCommandList*,int>,Session>sessions;
    std::recursive_mutex mutex;uint64_t evaluations=0,extendedCalls=0;bool restartRequired=false;
    std::string status;
    std::atomic<decltype(&NvAPI_D3D12_LaunchCuKernelChainEx)>launchEx{nullptr};
};
State& S(){static State value;return value;}
std::vector<NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX> captured;
const NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX* received=nullptr;
unsigned calls=0;
NvAPI_Status __cdecl Driver(ID3D12GraphicsCommandList*,const NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX*k,NvU32 n){
    auto worker=std::async(std::launch::async,[]{
        if(!S().mutex.try_lock())return false;
        S().mutex.unlock();return true;
    });
    if(!worker.get())throw std::runtime_error("driver called while NR lock held");
    ++calls;received=k;captured.assign(k,k+n);return NVAPI_OK;
}
#include "nr_reuse_api_production.inl"
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main() try {
    auto&s=S();s.launchEx=&Driver;
    const auto start=reinterpret_cast<NVDX_ObjectHandle>(1),finish=reinterpret_cast<NVDX_ObjectHandle>(2),other=reinterpret_cast<NVDX_ObjectHandle>(3);
    const auto module=reinterpret_cast<NVDX_ObjectHandle>(4);
    auto*cmd=reinterpret_cast<ID3D12GraphicsCommandList*>(5);int feature=0;
    s.vitRole.Add(start,module,DlssNrVitReuse::Role::Start);s.vitRole.Add(finish,module,DlssNrVitReuse::Role::End);
    int argument=23;void* pointers[]{&argument};
    NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX kernels[3]{};
    kernels[0].hFunction=start;kernels[1].hFunction=finish;kernels[2].hFunction=other;
    kernels[2].kernelParams=pointers;kernels[2].gridDim={4,3,2};kernels[2].blockDim={32,4,1};kernels[2].dynSharedMemBytes=77;
    // Startup must bypass the lock even when another thread owns it.
    { std::promise<void> locked,release;auto ready=release.get_future();
      std::thread owner([&]{std::lock_guard guard(s.mutex);locked.set_value();ready.wait();});
      locked.get_future().wait();
      // A separate forwarding stub does not probe the intentionally held lock.
      s.launchEx=+[](ID3D12GraphicsCommandList*,const NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX*,NvU32)->NvAPI_Status{return NVAPI_OK;};
      auto startup=std::async(std::launch::async,[&]{return LaunchEx(cmd,kernels,3);});
      bool nonblocking=startup.wait_for(std::chrono::seconds(1))==std::future_status::ready;
      release.set_value();owner.join();startup.get();s.launchEx=&Driver;
      check(nonblocking,"unrelated startup blocked on NR lock");
    }
    s.vit.Begin(&feature,false,2);Observation()={};
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && received==kernels && captured.size()==3,"warmup did not preserve original EX launch");
    check(s.vit.End() && s.vit.Computed()==1,"warmup not counted");
    s.vit.Begin(&feature,false,2);Observation()={};
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==1,"EX reuse did not filter the verified range");
    check(captured[0].kernelParams==pointers && captured[0].gridDim.x==4 && captured[0].gridDim.z==2 && captured[0].blockDim.y==4 && captured[0].dynSharedMemBytes==77,"EX launch metadata or pointer-array ABI changed");
    check(s.vit.End() && s.vit.Reused()==1,"EX reuse not counted");
    // Unrelated destruction must not invalidate the known model cache.
    if(s.vitRole.RemoveFunction(other))s.vit.Clear();
    check(!s.vitRole.RemoveModule(reinterpret_cast<NVDX_ObjectHandle>(99)),"unrelated module was classified as NR");
    check(s.vitRole.RemoveModule(module),"known module removal not detected");
    s.observedFunctions[start]={module,"cc_vit_1d_repack_2d_to_1d_fp8"};
    s.vit.Begin(&feature,false,2);Observation()={};
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && received==kernels && captured.size()==3,"unverified runtime was filtered");
    check(Observation().unknown==1 && Observation().known==0 && Observation().launches==3,"unverified launch was not diagnosed");s.vit.End();
    const auto previous=calls;s.sessions[{cmd,0}].pending=true;s.vit.Begin(&feature,false,2);
    check(LaunchEx(cmd,kernels,3)==NVAPI_ERROR && calls==previous && s.restartRequired,"pending hybrid pair allowed incompatible extended completion");
    puts("PASS: extracted EX wrapper preserves kernelParams/geometry, filters only verified kernels, diagnoses unknown modules and refuses a pending hybrid pair");
    return 0;
} catch(const std::exception&e){puts(e.what());return 1;}
