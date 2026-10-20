from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'OptiScaler/upscalers/IFeature.cpp').read_text()
body=src[src.index('void InstallFfxivNativeQuality(uintptr_t base)'):src.index('\nvoid EnsureFfxivNativeQuality()')]
arrays='\n'.join(re.findall(r'constexpr unsigned char (?:queryBytes|updateBytes|dispatchBytes)\[\] = \{.*?\};',body,re.S))
prefix=r'''
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <cassert>
#include <cstring>
#include <iostream>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
constexpr uintptr_t kFfxivQuerySizeRva=0x3520c0,kFfxivRendererUpdateRva=0x2da290,kFfxivResizeCallbacksRva=0x236a40;
std::atomic<bool> nativeQualityAvailable=false;
unsigned requests=0,begins=0,aborts=0,commits=0,attaches=0;int failAt=0;
using Fn=void(*)();Fn originalRendererUpdate=nullptr,dispatchResizeCallbacks=nullptr,originalQuerySize=nullptr;
using FfxivRendererUpdate=Fn;using FfxivResizeCallbacks=Fn;using FfxivQuerySize=Fn;
void ObserveFfxivQuery(){}void ObserveFfxivRendererUpdate(){}
struct Value{int value_or_default(){return 2;}};
struct Config{Value ForcePerfQuality;static Config* Instance(){static Config c;return &c;}};
namespace FfxivNativeQuality{void Request(int q){assert(q==2);++requests;}}
bool ReadFfxivMemory(const void* p,void* out,size_t n){memcpy(out,p,n);return true;}
LONG DetourTransactionBegin(){++begins;return failAt==1?ERROR_ACCESS_DENIED:NO_ERROR;}
LONG DetourUpdateThread(HANDLE){return failAt==2?ERROR_ACCESS_DENIED:NO_ERROR;}
LONG DetourAttach(PVOID*,Fn){++attaches;return failAt==3?ERROR_ACCESS_DENIED:NO_ERROR;}
LONG DetourTransactionCommit(){++commits;return failAt==4?ERROR_ACCESS_DENIED:NO_ERROR;}
LONG DetourTransactionAbort(){++aborts;return NO_ERROR;}
'''
main=r'''
int main(int argc,char** argv){
 int mode=argc>1?atoi(argv[1]):0;
 std::vector<unsigned char> image(0x400000);auto base=(uintptr_t)image.data();
 ARRAYS
 memcpy(image.data()+kFfxivQuerySizeRva,queryBytes,sizeof(queryBytes));
 memcpy(image.data()+kFfxivRendererUpdateRva,updateBytes,sizeof(updateBytes));
 memcpy(image.data()+kFfxivResizeCallbacksRva,dispatchBytes,sizeof(dispatchBytes));
 if(mode==5)image[kFfxivQuerySizeRva]^=1;
 failAt=mode;
 InstallFfxivNativeQuality(base);
 assert(nativeQualityAvailable==(mode==0));assert(requests==(mode==0?1:0));
 if(mode>=1 && mode<=4){
  assert(begins==1);if(mode==2 || mode==3)assert(aborts==1);
  failAt=0;InstallFfxivNativeQuality(base);assert(nativeQualityAvailable);assert(requests==1);
 }
 if(mode==5){assert(begins==0);for(int i=0;i<5;++i)InstallFfxivNativeQuality(base);assert(!nativeQualityAvailable);}
 auto before=begins;InstallFfxivNativeQuality(base);assert(begins==before);
 std::cout<<"PASS native quality installation: failure stage "<<mode<<", bounded retry and success state\n";
}
'''.replace('ARRAYS',arrays)
with tempfile.TemporaryDirectory(prefix='quality-hooks-',dir=root/'tests') as folder:
 p=Path(folder);(p/'test.cpp').write_text(prefix+body+main)
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',str(p/'test.cpp'),f'/Fo{p}/',f'/Fe{p}/test.exe'],check=True)
 for mode in range(6):subprocess.run([str(p/'test.exe'),str(mode)],check=True)
