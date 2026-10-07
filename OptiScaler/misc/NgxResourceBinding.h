#pragma once
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>

// Native NGX parameter blocks keep typed and void* resources in separate slots.
// The DX11 bridge writes the void* slot; preserve that slot when temporarily
// replacing a texture so NR and NGX see the same resource before and after SR.
namespace NgxResourceBinding
{
template <typename Parameters> struct Binding
{
    Parameters* parameters;
    const char* key;
    ID3D12Resource* original = nullptr;
    bool untyped = false;

    Binding(Parameters* params, const char* name) : parameters(params), key(name)
    {
        if (parameters->Get(key, &original) != NVSDK_NGX_Result_Success)
            original = nullptr;
        if (!original)
        {
            void* resource = nullptr;
            if (parameters->Get(key, &resource) == NVSDK_NGX_Result_Success)
            {
                original = static_cast<ID3D12Resource*>(resource);
                untyped = true;
            }
        }
    }

    void Set(ID3D12Resource* resource) const
    {
        if (untyped)
            parameters->Set(key, static_cast<void*>(resource));
        else
            parameters->Set(key, resource);
    }
    void Restore() const { Set(original); }
};
}
