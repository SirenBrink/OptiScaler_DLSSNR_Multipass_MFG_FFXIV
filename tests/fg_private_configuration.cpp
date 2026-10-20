// Optional integration test against a locally supplied, fingerprint-supported NVIDIA runtime.
// Executes NVIDIA configuration code; does not create a GPU model or establish visual success.
#include "pch.h"
#include <cassert>
#include <array>
#include "../OptiScaler/framegen/dlssg/PresetCache.cpp"
int ContextOption(void*,unsigned*){return 1;}
int wmain(int argc,wchar_t** argv){
 assert(argc==2 || argc==3);const wchar_t* path=argv[1];
 auto module=LoadLibraryW(path);assert(module);
 auto lifecycle=DlssgPresetCache::LocateLifecycle(module);assert(lifecycle);
 printf("Verified native lifecycle RVAs: create %llx copy %llx defaults %llx read %llx\n",lifecycle.create-(uintptr_t)module,lifecycle.copy-(uintptr_t)module,lifecycle.defaults-(uintptr_t)module,lifecycle.read-(uintptr_t)module);
 DlssgPresetCache::ObserveDriverQuery((void*)GetProcAddress(module,"NVSDK_NGX_D3D12_CreateFeature"));
 DlssgPreset::CaptureOriginal(0);
 if(argc==3){
  auto* instruction=reinterpret_cast<unsigned char*>(lifecycle.defaults);DWORD protection=0;
  assert(VirtualProtect(instruction,1,PAGE_EXECUTE_READWRITE,&protection));*instruction^=1;
  assert(!DlssgPresetCache::Ensure());
  *instruction^=1;DWORD ignored=0;assert(VirtualProtect(instruction,1,protection,&ignored));FlushInstructionCache(GetCurrentProcess(),instruction,1);
  puts("PASS modified endpoint instructions reject live switching before hooks install");return 0;
 }
 assert(DlssgPresetCache::Ensure());
 std::array<void*,56> vtable{};vtable[55]=(void*)ContextOption;struct Context{void** table;}context{vtable.data()};
 std::array<unsigned char,544> source,destination;source.fill(0xa5);destination.fill(0xab);
 DlssgPreset::Active().store(2);DlssgPresetCache::RefreshConfiguration(source.data()+16,&context);const auto saved=source;
 for(int requested:{1,3,2,0,3,1,0}){
  DlssgPresetCache::originalCopy(destination.data()+16,source.data()+16);
  DlssgPreset::Active().store(requested);DlssgPresetCache::RefreshConfiguration(destination.data()+16,&context);
  auto* config=destination.data()+16;
  if(requested){assert(config[0x54]==(requested==2));assert(config[0x55]==1);assert(config[0x68]==(requested==3));assert(config[0x69]==1);}
  else {assert(config[0x55]==0 && config[0x69]==0);}
  assert(source==saved);
  for(int i=0;i<16;++i){assert(destination[i]==0xab);assert(destination[528+i]==0xab);assert(source[i]==0xa5);assert(source[528+i]==0xa5);}
  printf("PASS native private config request %d: architecture values/overrides %u/%u %u/%u; source unchanged; bounds intact\n",requested,config[0x54],config[0x55],config[0x68],config[0x69]);
 }
 puts("PASS NVIDIA constructor and settings reader rebuild private A/B/C/Automatic configuration without retaining startup B flags");
}
