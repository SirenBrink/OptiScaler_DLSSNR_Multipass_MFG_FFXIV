from pathlib import Path
import tempfile, subprocess
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jitter-projection-',dir=r/'tests') as temp:
 b=Path(temp)
 (b/'pch.h').write_text('''#pragma once
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
struct State {std::string gameExe="another_game.exe";static State& Instance(){static State s;return s;}};
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
''')
 (b/'nvsdk_ngx.h').write_text('''#pragma once
#include <cstring>
constexpr int NVSDK_NGX_Result_Success=1;
constexpr auto NVSDK_NGX_Parameter_Jitter_Offset_X="X";
constexpr auto NVSDK_NGX_Parameter_Jitter_Offset_Y="Y";
constexpr auto NVSDK_NGX_Parameter_Width="W";
constexpr auto NVSDK_NGX_Parameter_Height="H";
constexpr auto NVSDK_NGX_Parameter_Reset="R";
struct NVSDK_NGX_Parameter {
 float x=.25f,y=-.125f;unsigned width=2560,height=1440,reset=0,writes=0;bool haveX=true,haveY=true;
 int Get(const char* key,float* v){bool xx=!std::strcmp(key,"X");if(!(xx?haveX:haveY))return 0;*v=xx?x:y;return 1;}
 int Get(const char* key,unsigned* v){*v=!std::strcmp(key,"W")?width:!std::strcmp(key,"H")?height:reset;return 1;}
 void Set(const char* key,float v){++writes;(!std::strcmp(key,"X")?x:y)=v;}
};
''')
 (b/'test.cpp').write_text('''#include "misc/FfxivJitter.cpp"
#include <cassert>
#include <limits>
#include <iostream>
#include <misc/TemporalSampling.h>
using namespace FfxivJitter;
void* GetterStub(uintptr_t){return nullptr;}
void Near(float a,float b){assert(std::abs(a-b)<1e-6f);}
void Snapshot(float scale=1){
 samples={};cursor=1;
 samples[0]={.25f,-.125f,2*.25f*scale/2560,-2*(-.125f)*scale/1440,2560,1440,GetTickCount64()};
}
int main(){
 Install();assert(!original); // other games never get a native executable hook
 original=GetterStub;
 float x=0,y=0;Snapshot();assert(Pixels(samples[0],x,y));Near(x,.25f);Near(y,-.125f);
 for(float scale:{.5f,1.f,2.f}){
  Snapshot(scale);NVSDK_NGX_Parameter p;
  {Binding bind(&p);assert(matched);Near(p.x,.25f*scale);Near(p.y,-.125f*scale);}
  Near(p.x,.25f);Near(p.y,-.125f);assert(!matched);
  assert(p.writes==(scale==1?0:4));
 }
 Snapshot(.5f);NVSDK_NGX_Parameter p;
 try{Binding bind(&p);throw 7;}catch(int){}Near(p.x,.25f);Near(p.y,-.125f);
 Snapshot(.5f);samples[0].ndcY*=-1;p.writes=0;{Binding bind(&p);assert(!matched);}assert(!p.writes);
 Snapshot(.5f);samples[0].time=GetTickCount64()-2000;{Binding bind(&p);assert(!matched);}assert(!p.writes);
 Snapshot(.5f);p.width=1920;{Binding bind(&p);assert(!matched);}assert(!p.writes);p.width=2560;
 Snapshot(.5f);p.haveY=false;{Binding bind(&p);assert(!matched);}assert(!p.writes);p.haveY=true;
 Snapshot(.5f);p.x=-.25f;{Binding bind(&p);assert(!matched);}assert(!p.writes);p.x=.25f;
 Snapshot(.5f);samples[0].width=0;assert(!Pixels(samples[0],x,y));
 Snapshot(.5f);samples[0].ndcX=std::numeric_limits<float>::infinity();assert(!Pixels(samples[0],x,y));
 Snapshot(.5f);samples[0].ndcY=std::numeric_limits<float>::quiet_NaN();assert(!Pixels(samples[0],x,y));
 Snapshot(.5f);samples[0].rawX=samples[0].rawY=0;assert(Pixels(samples[0],x,y));Near(x,0);Near(y,0);
 // Native FFXIV scales the Halton lattice; normalize before the matrix math,
 // then verify the raster and queued NGX sample retain the original full pixel coverage.
 float maxX=0,maxY=0;
 for(float strength:{.3f,.6f,1.f,1.5f})for(unsigned i=1;i<=512;++i){
  float hx=TemporalSampling::Halton(i,2),hy=TemporalSampling::Halton(i,3);
  float sx=hx,sy=hy;NormalizeHalton(sx,sy,strength);
  Sample projected{sx,sy,2*sx*strength/2560,-2*sy*strength/1440,2560,1440,GetTickCount64()};
  assert(Pixels(projected,x,y));Near(x,hx);Near(y,hy);
  maxX=std::max(maxX,std::abs(x));maxY=std::max(maxY,std::abs(y));
  samples={};samples[0]=projected;cursor=1;
  NVSDK_NGX_Parameter queued;queued.x=x;queued.y=y;
  {Binding bind(&queued);assert(matched);Near(queued.x,hx);Near(queued.y,hy);}
  assert(queued.writes==0); // same-frame packet already contains exactly the raster offset
 }
 assert(maxX>.49f && maxY>.49f);
 for(float strength:{0.f,.01f,-1.f,5.f,std::numeric_limits<float>::infinity()}){
  float a=.25f,b=-.125f;assert(!NormalizeHalton(a,b,strength));Near(a,.25f);Near(b,-.125f);
 }

 // Finite native pattern repeats with the engine counter and scales with the
 // true render/output pixel area, including FFXIV's rounded Balanced extent.
 for(auto dims: {std::array<unsigned,4>{3840,2160,3840,2160},
                 std::array<unsigned,4>{2560,1440,3840,2160},
                 std::array<unsigned,4>{2257,1270,3840,2160}}){
   unsigned phases=PatternPhases(0xffffffffu,dims[0],dims[1],dims[2],dims[3]);
   assert(phases==TemporalSampling::PhaseCount(dims[0],dims[1],dims[2],dims[3]));
   for(unsigned frame=0;frame<phases*4;++frame){
      unsigned phase=frame%phases+1;
      float hx=TemporalSampling::Halton(phase,2),hy=TemporalSampling::Halton(phase,3);
      float sx=hx,sy=hy;NormalizeHalton(sx,sy,.6f);
      Sample projected{sx,sy,2*sx*.6f/dims[0],-2*sy*.6f/dims[1],dims[0],dims[1],GetTickCount64()};
      assert(Pixels(projected,x,y));Near(x,hx);Near(y,hy);
      assert(std::abs(x)<=.5f && std::abs(y)<=.5f);
   }
 }
 assert(PatternPhases(0xffffffffu,3840,2160,3840,2160)==8);
 assert(PatternPhases(0xffffffffu,2560,1440,3840,2160)==18);
 assert(PatternPhases(0xffffffffu,2257,1270,3840,2160)==24);
 assert(PatternPhases(16,2560,1440,3840,2160)==16); // non-DLSS sequence untouched
 assert(PatternPhases(0xffffffffu,0,1440,3840,2160)==0xffffffffu);
 assert(PatternPhases(0xffffffffu,7680,4320,3840,2160)==0xffffffffu);
 original=nullptr;
 std::cout<<"PASS native Halton coverage normalization and same-frame packet matching\\n";
 std::cout<<"PASS production jitter binding: native projection units/sign, matching, correction, unchanged valid input, stale/invalid/mismatched rejection, exception restoration\\n";
}
''')
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',f'/I{r/"OptiScaler"}',f'/I{r/"OptiScaler/include"}',str(b/'test.cpp'),f'/Fo{b}/',f'/Fe{b}/test.exe',str(r/'OptiScaler/library/detours/detours.lib')],check=True)
 subprocess.run([str(b/'test.exe')],check=True)
