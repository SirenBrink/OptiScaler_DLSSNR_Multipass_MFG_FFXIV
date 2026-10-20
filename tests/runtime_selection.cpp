#include <cassert>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <dlssnr/RuntimeSelection.h>
#include <framegen/dlssg/PresetSelection.h>
using NvU32=uint32_t;
constexpr NvU32 NGX_DLSS_FG_OVERRIDE_ID=0x10E41E03;
constexpr NvU32 NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID=0x10E41DF1;
constexpr NvU32 NGX_DLSS_FG_OVERRIDE_ON=1, NVDRS_DWORD_TYPE=0;
struct NVDRS_SETTING {NvU32 settingType=NVDRS_DWORD_TYPE;NvU32 u32CurrentValue=99;};
void ApplyPreset(NvU32 settingId,NVDRS_SETTING* pSetting) {
#include "preset_override.inl"
}
int main(int argc,char**argv) {
    assert(argc==3);std::filesystem::path root=argv[1];std::string mode=argv[2];
    std::filesystem::create_directories(root/"OptiScaler"/"NR.310.8.0.0");
    std::filesystem::create_directories(root/"OptiScaler"/"NR.310.9.16.0");
    for(const char* v:{"NR.310.8.0.0","NR.310.9.16.0"}) std::ofstream(root/"OptiScaler"/v/"nvngx_dlssnr.dll");
    Config::Instance()->MainDllPath=root/"OptiScaler";Util::base=root;
    auto roots=DlssNr::RuntimeSelection::Roots();
    auto versions=DlssNr::RuntimeCatalog::Scan(roots);assert(versions.size()==2);
    for(const char* bad:{"..","../NR.310.8.0.0","NR...","NR.310/8","NR.310.8.","NR.310:8","NR.310.8\\dll"}) {
        assert(!DlssNr::RuntimeCatalog::Valid(bad));assert(!DlssNr::RuntimeCatalog::Resolve(roots,bad));
    }
    assert(!DlssNr::RuntimeCatalog::Legacy(root)); // catalog never chosen by an automatic recursive search
    std::ofstream(root/"nvngx_dlssnr.dll");
    auto& c=*Config::Instance();int expected=mode=="legacy"?0:mode=="preset-a"?1:mode=="preset-b"?2:3;
    c.FGDLSSGRenderPreset=expected;
    if(mode=="explicit")c.DlssNrRuntime="NR.310.9.16.0";
    else if(mode=="missing")c.DlssNrRuntime="NR.999.0.0.0";
    const auto initial=DlssNr::RuntimeSelection::Current();
    const auto first=DlssNr::RuntimeSelection::Path();
    const auto startup=DlssNr::RuntimeSelection::Startup().path;
    if(mode=="missing")assert(!first); // loose legacy file cannot replace a missing choice
    else if(mode=="explicit")assert(first==root/"OptiScaler"/"NR.310.9.16.0"/"nvngx_dlssnr.dll");
    else assert(first==root/"nvngx_dlssnr.dll");
    assert(DlssgPreset::Startup()==expected);
    NVDRS_SETTING enable,preset,other,wrongType;wrongType.settingType=2;
    ApplyPreset(NGX_DLSS_FG_OVERRIDE_ID,&enable);
    ApplyPreset(NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID,&preset);
    ApplyPreset(12345,&other);ApplyPreset(NGX_DLSS_FG_OVERRIDE_ID,&wrongType);
    assert(enable.u32CurrentValue==(expected?1:99));
    assert(preset.u32CurrentValue==(expected?expected:99));
    assert(other.u32CurrentValue==99 && wrongType.u32CurrentValue==99);
    c.FGDLSSGRenderPreset=expected==1?2:1;assert(DlssgPreset::Startup()==expected);
    c.DlssNrRuntime="NR.310.8.0.0";
    auto changed=DlssNr::RuntimeSelection::Current();
    assert(changed->revision==initial->revision+1);
    assert(changed->path==root/"OptiScaler"/"NR.310.8.0.0"/"nvngx_dlssnr.dll");
    assert(DlssNr::RuntimeSelection::Current()==changed);
    assert(DlssNr::RuntimeSelection::Startup().path==startup);
    assert(initial->path==first); // immutable old snapshots remain valid during GPU retirement
    c.DlssNrRuntime="NR.999.0.0.0";auto missing=DlssNr::RuntimeSelection::Current();
    assert(!missing->path && missing->revision==changed->revision+1);
    c.DlssNrRuntime="NR.310.9.16.0";assert(DlssNr::RuntimeSelection::Path()==root/"OptiScaler"/"NR.310.9.16.0"/"nvngx_dlssnr.dll");
    c.DlssNrPrecision.value=4;assert(DlssNr::RuntimeSelection::Path()==startup);
    c.DlssNrPrecision.value=0;c.DlssNrUseProxy.value=1;assert(DlssNr::RuntimeSelection::Path()==startup);
    c.DlssNrUseProxy.value=0;assert(DlssNr::RuntimeSelection::Path()==DlssNr::RuntimeSelection::Current()->path);
    int stops=0,frees=0;
    for(int next:{1,2,3,0,3,2,1,0}){
        const int old=DlssgPreset::Current();
        const auto changed=DlssgPreset::Apply(next,[&]{++stops;assert(DlssgPreset::Current()==old);return true;},[&]{++frees;assert(DlssgPreset::Current()==old);return true;});
        assert(changed==(next==old?DlssgPreset::Change::Unchanged:DlssgPreset::Change::Recreated));
        assert(DlssgPreset::Current()==next);
        NVDRS_SETTING setting;ApplyPreset(NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID,&setting);
        assert(setting.u32CurrentValue==(next?next:99));
    }
    const int beforeStop=stops,beforeFree=frees;
    assert(DlssgPreset::Apply(1,[&]{++stops;return false;},[&]{++frees;return true;})==DlssgPreset::Change::RestartRequired);
    assert(DlssgPreset::Current()==0 && stops==beforeStop+1 && frees==beforeFree);
    assert(DlssgPreset::Apply(1,[&]{++stops;return true;},[&]{++frees;return true;})==DlssgPreset::Change::RestartRequired);
    assert(stops==beforeStop+1); // failed requests do not retry every frame
    assert(DlssgPreset::Apply(2,[]{return true;},[]{return false;})==DlssgPreset::Change::RestartRequired);
    assert(DlssgPreset::Current()==0);
    assert(DlssgPreset::Apply(0,[]{assert(false);return true;},[]{assert(false);return true;})==DlssgPreset::Change::Unchanged);

    std::cout<<"PASS: "<<mode<<" resolution, no silent fallback, live D3D12 changes, immutable snapshots, startup search-path pinning, catalog deduplication and path validation\n";
}
