#include "pch.h"
#include "PresetCache.h"
#include "PresetSelection.h"
#include <detours/detours.h>
#include <bcrypt.h>
#include <TlHelp32.h>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <cwctype>
#include <intrin.h>
#include <array>
namespace DlssgPresetCache { namespace {
using Read=bool(*)(unsigned,unsigned*);
Read original=nullptr;
using Create=unsigned(*)(void*,void*,void*,void*,void*,void*);
using Copy=void(*)(void*,const void*);
using Defaults=void(*)(void*);
using ReadConfiguration=bool(*)(void*,void*);
Create originalCreate=nullptr;
Copy originalCopy=nullptr;
Defaults initializeConfiguration=nullptr;
ReadConfiguration readConfiguration=nullptr;
std::array<uintptr_t,2> configurationCopies{};
thread_local void* creationContext=nullptr;

// The DLL caches one shared configuration for the process. Releasing a viewport
// creates another endpoint, but ordinarily just copies that startup configuration.
// Rebuild only the new endpoint's private copy through NVIDIA's own constructor
// and settings reader. Existing endpoints and the shared configuration are untouched.
void RefreshConfiguration(void* destination,void* context){
    initializeConfiguration(destination);
    readConfiguration(destination,context);
}
void CopyHook(void* destination,const void* source){
    originalCopy(destination,source);
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(creationContext && (caller==configurationCopies[0] || caller==configurationCopies[1])){
        RefreshConfiguration(destination,creationContext);
        LOG_INFO("DLSS-G: refreshed new endpoint configuration for {}",DlssgPreset::Name(DlssgPreset::Current()));
    }
}
unsigned CreateHook(void* endpoint,void* context,void* a3,void* a4,void* a5,void* a6){
    struct Scope {void* previous=creationContext;explicit Scope(void* value){creationContext=value;}~Scope(){creationContext=previous;}} scope(context);
    return originalCreate(endpoint,context,a3,a4,a5,a6);
}
struct Lifecycle {
    uintptr_t create=0,copy=0,defaults=0,read=0;
    std::array<uintptr_t,2> copies{};
    explicit operator bool()const{return create && copy && defaults && read && copies[0] && copies[1];}
};
Lifecycle LocateLifecycle(HMODULE module){
    const auto base=reinterpret_cast<uintptr_t>(module);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    const auto& exception=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto* functions=reinterpret_cast<const RUNTIME_FUNCTION*>(base+exception.VirtualAddress);
    const auto count=exception.Size/sizeof(RUNTIME_FUNCTION);
    auto owner=[&](uintptr_t address)->uintptr_t{
        if(address<base)return 0;
        const auto rva=address-base;
        for(size_t i=0;i<count;++i)if(functions[i].BeginAddress<=rva && rva<functions[i].EndAddress)return base+functions[i].BeginAddress;
        return 0;
    };
    auto endOf=[&](uintptr_t address)->uintptr_t{
        for(size_t i=0;i<count;++i)if(base+functions[i].BeginAddress==address)return base+functions[i].EndAddress;
        return 0;
    };
    Lifecycle result;
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    constexpr unsigned char readCall[]={0x48,0x8d,0x54,0x24,0x68,0xc7,0x44,0x24,0x68,0,0,0,0,0xb9,0xf1,0x1d,0xe4,0x10,0xe8};
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){
        const auto& section=sections[i];if(!(section.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        const auto* bytes=reinterpret_cast<const unsigned char*>(base+section.VirtualAddress);
        for(size_t j=0;j+sizeof(readCall)<=section.Misc.VirtualSize;++j){
            if(std::memcmp(bytes+j,readCall,sizeof(readCall)))continue;
            if(result.read)return {};result.read=owner(reinterpret_cast<uintptr_t>(bytes+j));
        }
    }
    constexpr unsigned char readStart[]={0x40,0x53,0x56,0x57,0x48,0x83,0xec,0x40};
    if(!result.read || std::memcmp(reinterpret_cast<const void*>(result.read),readStart,sizeof(readStart)))return {};
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){
        const auto& section=sections[i];if(!(section.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        const auto* bytes=reinterpret_cast<const unsigned char*>(base+section.VirtualAddress);
        for(size_t j=0;j+5<=section.Misc.VirtualSize;++j){
            if(bytes[j]!=0xe8)continue;int32_t displacement=0;std::memcpy(&displacement,bytes+j+1,4);
            const auto caller=reinterpret_cast<uintptr_t>(bytes+j);
            if(caller+5+displacement!=result.read)continue;
            if(result.create)return {};result.create=owner(caller);
        }
    }
    constexpr unsigned char createStart[]={0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x70,0xfc,0xff,0xff};
    if(!result.create || std::memcmp(reinterpret_cast<const void*>(result.create),createStart,sizeof(createStart)))return {};
    const auto end=endOf(result.create);
    constexpr unsigned char prefix[]={0x48,0x8d,0x4d,0x40,0xe8};
    constexpr unsigned char defaultsStart[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x55,0x48,0x8b,0xec,0x48,0x83,0xec,0x20,0x41,0xbb,0x03,0,0,0};
    constexpr unsigned char copyStart[]={0x8b,0x02,0x89,0x01,0x0f,0xb7,0x42,0x04,0x66,0x89,0x41,0x04,0x8b,0x42,0x14,0x89,0x41,0x14};
    size_t copies=0;
    for(uintptr_t at=result.create;at+9<=end;++at){
        if(std::memcmp(reinterpret_cast<const void*>(at),prefix,sizeof(prefix)))continue;
        int32_t displacement=0;std::memcpy(&displacement,reinterpret_cast<const void*>(at+5),4);
        const auto target=at+9+displacement;
        // Targets must stay in the same executable section as the endpoint.
        bool executable=false;
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto& section=sections[i];
            if((section.Characteristics&IMAGE_SCN_MEM_EXECUTE) && target>=base+section.VirtualAddress && target+sizeof(defaultsStart)<=base+section.VirtualAddress+section.Misc.VirtualSize)executable=true;}
        if(!executable)return {};
        if(!std::memcmp(reinterpret_cast<const void*>(target),defaultsStart,sizeof(defaultsStart))){
            if(result.defaults)return {};result.defaults=target;
        }else if(!std::memcmp(reinterpret_cast<const void*>(target),copyStart,sizeof(copyStart))){
            if(copies>=2 || (result.copy && result.copy!=target))return {};
            result.copy=target;result.copies[copies++]=at+9;
        }
    }
    return result;
}
std::atomic<HMODULE> observedProvider{nullptr};
enum class Status { Unknown, Ready, MissingProvider, UnsupportedProvider, BadSignature, Failed };
std::atomic<Status> status{Status::Unknown};
bool Hook(unsigned id,unsigned* value){
    const int requested=DlssgPreset::Current();
    if(Override(id,value,requested,DlssgPreset::originalPreset.load(std::memory_order_acquire))){
        DlssgPreset::querySeen.store(true,std::memory_order_relaxed);
        static std::atomic<int> reported{-1};
        if(reported.exchange(requested)!=requested)
            LOG_INFO("DLSS-G cached preset read: {} -> {}",DlssgPreset::Name(requested),*value);
        return true;
    }
    return original(id,value);
}
bool Candidate(HMODULE module){
    if(!GetProcAddress(module,"NVSDK_NGX_D3D12_CreateFeature"))return false;
    wchar_t path[32768]{};if(!GetModuleFileNameW(module,path,32768))return false;
    std::wstring name(path);std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return std::towlower(c);});
    return name.find(L"dlssg")!=std::wstring::npos;
}
bool Supported(HMODULE module){
    wchar_t path[32768]{};if(!GetModuleFileNameW(module,path,32768))return false;
    std::ifstream in(std::filesystem::path(path),std::ios::binary|std::ios::ate);
    if(!in)return false;const auto size=in.tellg();if(size<=0 || size>128*1024*1024)return false;
    std::vector<unsigned char> bytes(static_cast<size_t>(size));in.seekg(0);if(!in.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))return false;
    constexpr unsigned char expected[]={0x8d,0x59,0xd4,0x9b,0x36,0x41,0x0d,0xd6,0xb6,0x2f,0xef,0x25,0x03,0xa5,0x87,0x1d,0x34,0x74,0x15,0xbd,0x85,0xd9,0x43,0xfa,0x08,0x48,0x56,0x3a,0x95,0xdd,0x50,0xc7};
    constexpr unsigned char driverManaged[]={0xca,0xde,0xe1,0xaa,0x6d,0xb3,0x90,0x7b,0xab,0xdc,0x32,0x85,0xda,0x2a,0xeb,0xc4,0x55,0x3d,0x92,0xb8,0x9c,0x38,0xba,0x3e,0xae,0x8a,0xbd,0x8b,0xba,0x1b,0xea,0x72};
    unsigned char digest[32]{};
    return BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,bytes.data(),ULONG(bytes.size()),digest,32)==0 && (!std::memcmp(digest,expected,32) || !std::memcmp(digest,driverManaged,32));
}
}
void ObserveDriverQuery(void* caller){
    HMODULE owner=nullptr;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(caller),&owner) &&
        Candidate(owner))
        observedProvider.store(owner,std::memory_order_release);
}
const char* UnavailableReason(){
    switch(status.load()){
        case Status::MissingProvider:return "the active DLSS-G runtime could not be identified";
        case Status::UnsupportedProvider:return "the loaded DLSS-G runtime is not supported for live changes";
        case Status::BadSignature:return "the loaded DLSS-G runtime did not pass verification";
        default:return "DLSS-G could not reset its resources safely";
    }
}
bool Ensure(){
    static std::mutex mutex;std::lock_guard lock(mutex);
    if(original)return true;
    static bool attempted=false;if(attempted)return false;attempted=true;
    HMODULE module=observedProvider.load(std::memory_order_acquire);
    if(!module){
        // Multiple DLLs with the same basename can coexist; the actual provider
        // may also be a driver-managed .bin image. Never trust GetModuleHandle(name).
        HANDLE modules=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
        if(modules!=INVALID_HANDLE_VALUE){
            MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);bool ambiguous=false;
            for(BOOL next=Module32FirstW(modules,&entry);next;next=Module32NextW(modules,&entry)){
                if(!Candidate(entry.hModule) || !Supported(entry.hModule))continue;
                if(module){ambiguous=true;break;}module=entry.hModule;
            }
            CloseHandle(modules);if(ambiguous)module=nullptr;
        }
    }
    if(!module){status.store(Status::MissingProvider);LOG_WARN("DLSS-G live presets: active provider unidentified; live change blocked");return false;}
    wchar_t providerPath[32768]{};GetModuleFileNameW(module,providerPath,32768);
    LOG_INFO("DLSS-G live preset provider: {}",std::filesystem::path(providerPath).string());
    if(!Supported(module)){status.store(Status::UnsupportedProvider);LOG_WARN("DLSS-G live presets: active provider is unverified; live change blocked");return false;}
    status.store(Status::Failed);
    auto base=reinterpret_cast<uintptr_t>(module);const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    uintptr_t address=0;
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){
        const auto& section=sections[i];if(!(section.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        const auto found=FindReader({reinterpret_cast<const unsigned char*>(base+section.VirtualAddress),section.Misc.VirtualSize});
        if(found){if(address)return false;address=found;}
    }
    if(!address){status.store(Status::BadSignature);LOG_WARN("DLSS-G live presets: cached-reader guards failed; restart required");return false;}
    const auto lifecycle=LocateLifecycle(module);
    if(!lifecycle){status.store(Status::BadSignature);LOG_WARN("DLSS-G live presets: endpoint configuration guards failed; live change blocked");return false;}
    HMODULE retained=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(address),&retained))return false;
    std::vector<HANDLE> threads;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE){FreeLibrary(retained);return false;}
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);bool next=Thread32First(snapshot,&entry)!=FALSE,ready=next;
    while(next){
        if(entry.th32OwnerProcessID==GetCurrentProcessId() && entry.th32ThreadID!=GetCurrentThreadId()){
            HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,entry.th32ThreadID);
            if(thread)threads.push_back(thread);else if(GetLastError()!=ERROR_INVALID_PARAMETER)ready=false;
        }
        next=Thread32Next(snapshot,&entry)!=FALSE;
    }
    CloseHandle(snapshot);
    if(!ready || DetourTransactionBegin()!=NO_ERROR){for(auto t:threads)CloseHandle(t);FreeLibrary(retained);return false;}
    original=reinterpret_cast<Read>(address);
    originalCreate=reinterpret_cast<Create>(lifecycle.create);
    originalCopy=reinterpret_cast<Copy>(lifecycle.copy);
    initializeConfiguration=reinterpret_cast<Defaults>(lifecycle.defaults);
    readConfiguration=reinterpret_cast<ReadConfiguration>(lifecycle.read);
    configurationCopies=lifecycle.copies;
    bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR;
    for(auto t:threads)ok &= DetourUpdateThread(t)==NO_ERROR;
    if(ok)ok=DetourAttach(reinterpret_cast<PVOID*>(&original),Hook)==NO_ERROR;
    if(ok)ok=DetourAttach(reinterpret_cast<PVOID*>(&originalCreate),CreateHook)==NO_ERROR;
    if(ok)ok=DetourAttach(reinterpret_cast<PVOID*>(&originalCopy),CopyHook)==NO_ERROR;
    LONG result=ERROR_INVALID_FUNCTION;if(ok)result=DetourTransactionCommit();else DetourTransactionAbort();
    for(auto t:threads)CloseHandle(t);
    if(result!=NO_ERROR){original=nullptr;originalCreate=nullptr;originalCopy=nullptr;initializeConfiguration=nullptr;readConfiguration=nullptr;FreeLibrary(retained);return false;}
    // The reader trampoline and its module remain paired until process exit.
    status.store(Status::Ready);LOG_INFO("DLSS-G live presets: verified cached-settings and private endpoint configuration hooks installed");return true;
}
}
