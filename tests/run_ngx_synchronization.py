from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1]
source=r'''
#include <misc/NgxSynchronization.h>
#include <atomic>
#include <cassert>
#include <thread>
#include <future>
#include <vector>
#include <iostream>
std::atomic<int> inCall{0};
int Runtime(int x){assert(inCall.fetch_add(1)==0);std::this_thread::yield();assert(inCall.fetch_sub(1)==1);return x+7;}
struct Module{int(*create)(int)=Runtime;int(*release)(int)=Runtime;};inline Module module;
int main(){
 using namespace NgxSynchronization;
 // Compile and exercise the same pointer-to-module-member wrapper as the proxy.
 std::vector<std::thread> workers;
 for(int i=0;i<4;++i) workers.emplace_back([]{for(int n=0;n<500;++n){
   assert(Serialized<&module.create>::Call(n)==n+7);assert(Serialized<&module.release>::Call(n)==n+7);
 }});
 for(auto& t:workers)t.join();assert(inCall==0);
 std::promise<void> started,entered;auto startedFuture=started.get_future(),enteredFuture=entered.get_future();
 std::thread release;
 {
   FeatureGuard evaluation(dx11Features);
   {FeatureGuard recursiveCallback(dx11Features);}
   release=std::thread([&]{started.set_value();FeatureGuard retirement(dx11Features);entered.set_value();});
   startedFuture.wait();assert(enteredFuture.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
 }
 enteredFuture.get();release.join();
 std::cout<<"PASS 4000 concurrent NGX calls serialized with preserved returns; feature release waits for evaluate; nested resize callbacks do not deadlock\n";
}
'''
with tempfile.TemporaryDirectory(prefix='ngx-sync-',dir=r/'tests') as folder:
 p=Path(folder);(p/'test.cpp').write_text(source)
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{r/"OptiScaler"}',str(p/'test.cpp'),f'/Fo{p}/',f'/Fe{p}/test.exe'],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
# Guard coverage for the production entry points that own the shared feature map.
s=(r/'OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp').read_text()
for name in ['CreateFeature','ReleaseFeature','EvaluateFeature','Shutdown','Shutdown1']:
 start=s.index('NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_'+name+'(')
 brace=s.index('{',start)
 assert 'NgxSynchronization::FeatureGuard' in s[brace:brace+150],name
print('PASS production create/evaluate/release/shutdown lifetime guard coverage')
