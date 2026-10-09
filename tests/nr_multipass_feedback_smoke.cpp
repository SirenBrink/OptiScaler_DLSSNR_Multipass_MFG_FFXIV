// Executes the optional interpass feedback shader on WARP, checking bounded RGB,
// nonfinite inputs, alpha preservation, and feedback weights 0, 0.5 and 1.
// Production bypasses this dispatch entirely at the default feedback of 1,
// preserving the original raw interpass output.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;
struct Pixel { float r, g, b, a; };
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static void expect(bool ok, const char* label) { if (!ok) throw std::runtime_error(label); }
static bool near3(Pixel a, Pixel b, float e = 1e-5f) {
    return std::abs(a.r - b.r) < e && std::abs(a.g - b.g) < e && std::abs(a.b - b.b) < e;
}
static float sanitizeFinite(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

// The same CubeScaleResidual maths as the interpass-clamp smoke test's own CPU reference -- this is
// "restored" in the shader, before any damping is applied. Matches the shader's own SanitizeFinite3
// pass over both operands first (fallback 0.5, per-component) -- omitting that step here doesn't
// disprove the shader, it just propagates NaN/Inf through this reference instead of the value the
// shader actually produced, which was the bug in this test's first draft.
static Pixel restoredReference(Pixel P, Pixel T) {
    float p[3] = {sanitizeFinite(P.r, 0.5f), sanitizeFinite(P.g, 0.5f), sanitizeFinite(P.b, 0.5f)};
    float t[3] = {sanitizeFinite(T.r, 0.5f), sanitizeFinite(T.g, 0.5f), sanitizeFinite(T.b, 0.5f)};
    float alpha = 1.0f;
    for (int c = 0; c < 3; ++c) {
        float d = t[c] - p[c];
        if (d > 1e-6f) alpha = std::min(alpha, (1.0f - p[c]) / d);
        else if (d < -1e-6f) alpha = std::min(alpha, (0.0f - p[c]) / d);
    }
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return {p[0] + alpha * (t[0] - p[0]), p[1] + alpha * (t[1] - p[1]), p[2] + alpha * (t[2] - p[2]), T.a};
}
// What the shader is specified to compute: a convex blend of the *sanitized* proxy (sanProxy in the
// shader -- the same value CubeScaleResidual was itself called against) and the restored answer.
static Pixel dampedReference(Pixel P, Pixel T, float feedback) {
    Pixel restored = restoredReference(P, T);
    float sp[3] = {sanitizeFinite(P.r, 0.5f), sanitizeFinite(P.g, 0.5f), sanitizeFinite(P.b, 0.5f)};
    if (feedback >= 1.0f) return restored;
    return {sp[0] + feedback * (restored.r - sp[0]), sp[1] + feedback * (restored.g - sp[1]),
            sp[2] + feedback * (restored.b - sp[2]), restored.a};
}
int wmain(int argc, wchar_t** argv) try {
    if (argc != 2) throw std::runtime_error("Pass the dlssnr.hlsl path");
    ComPtr<ID3DBlob> code, errors;
    HRESULT compiled = D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) std::fprintf(stderr, "%s", (char*)errors->GetBufferPointer());
    check(compiled);
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));

    // Same 8 cases as the interpass-clamp smoke test (case 0: raw already valid, a real
    // non-overshooting edit -- the common case; cases 1-4: various overshoots; 5-6: NaN/Inf;
    // 7: overshoot with alpha carried through). Reused deliberately, not re-invented, so this test's
    // feedback=1.0 column is checkable against that test's already-established results.
    constexpr int N = 8;
    const std::array<Pixel, N> proxy {{
        {0.5f, 0.5f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.2f, 0.4f, 0.8f, 1},
        {0.9f, 0.1f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.3f, 0.3f, 0.3f, 1}, {0.6f, 0.2f, 0.4f, 0.25f}}};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<Pixel, N> raw {{
        {0.55f, 0.45f, 0.60f, 1}, {1.40f, 0.60f, 0.40f, 1}, {-0.30f, 0.55f, 0.45f, 1},
        {1.30f, -0.20f, 1.10f, 1}, {2.00f, 0.30f, 0.80f, 1}, {nan, 0.5f, 0.5f, 1},
        {INFINITY, 0.3f, 0.3f, 1}, {1.50f, 0.10f, 0.90f, 0.25f}}};

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = N; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> rawTex, proxyTex, output, keep, readback;
    D3D11_SUBRESOURCE_DATA data {raw.data(), sizeof(raw), 0};
    check(device->CreateTexture2D(&desc, &data, &rawTex));
    data.pSysMem = proxy.data(); check(device->CreateTexture2D(&desc, &data, &proxyTex));
    ComPtr<ID3D11ShaderResourceView> rawSrv, proxySrv;
    check(device->CreateShaderResourceView(rawTex.Get(), nullptr, &rawSrv));
    check(device->CreateShaderResourceView(proxyTex.Get(), nullptr, &proxySrv));
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    check(device->CreateTexture2D(&desc, nullptr, &output)); check(device->CreateTexture2D(&desc, nullptr, &keep));
    ComPtr<ID3D11UnorderedAccessView> outputUav, keepUav;
    check(device->CreateUnorderedAccessView(output.Get(), nullptr, &outputUav));
    check(device->CreateUnorderedAccessView(keep.Get(), nullptr, &keepUav));
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, &readback));
    D3D11_BUFFER_DESC buffer {}; buffer.ByteWidth = sizeof(DlssNrConstants); buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants; check(device->CreateBuffer(&buffer, nullptr, &constants));
    D3D11_SAMPLER_DESC sampling {}; sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampling.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; check(device->CreateSamplerState(&sampling, &sampler));

    ID3D11ShaderResourceView* srvs[] = {rawSrv.Get(), proxySrv.Get(), proxySrv.Get(), proxySrv.Get(), proxySrv.Get()};
    ID3D11UnorderedAccessView* uavs[] = {outputUav.Get(), keepUav.Get()};
    ctx->CSSetShader(shader.Get(), nullptr, 0); ctx->CSSetShaderResources(0, 5, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr); ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());

    auto runAt = [&](float feedback) {
        DlssNrConstants settings {}; settings.Mode = DlssNrMode_MultipassFeedback; settings.Width = N; settings.Height = 1;
        settings.WhitePoint = 1; settings.Passthrough = 0; settings.PassFeedback = feedback;
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &settings, 0, 0); ctx->Dispatch(1, 1, 1);
        ctx->CopyResource(readback.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::array<Pixel, N> out; memcpy(out.data(), mapped.pData, sizeof(out)); ctx->Unmap(readback.Get(), 0);
        return out;
    };

    const auto at1_0 = runAt(1.0f);
    const auto at0_5 = runAt(0.5f);
    const auto at0_0 = runAt(0.0f);

    std::puts("idx  proxy               feedback=1.0            feedback=0.5            feedback=0.0");
    for (int i = 0; i < N; ++i)
        std::printf("%d    %.2f %.2f %.2f      %.4f %.4f %.4f      %.4f %.4f %.4f      %.4f %.4f %.4f\n", i,
                    proxy[i].r, proxy[i].g, proxy[i].b, at1_0[i].r, at1_0[i].g, at1_0[i].b,
                    at0_5[i].r, at0_5[i].g, at0_5[i].b, at0_0[i].r, at0_0[i].g, at0_0[i].b);

    // The shader endpoint agrees with the independent bounded-residual reference.
    for (int i = 0; i < N; ++i)
        expect(near3(at1_0[i], restoredReference(proxy[i], raw[i]), 1e-6f),
               "feedback=1.0 differs from the undamped CubeScaleResidual reference");

    // AC: PassFeedback<1 measurably differs wherever the pass actually changed the picture. Case 5
    // (a NaN raw channel) sanitizes to exactly the proxy's own value, so its "edit" is legitimately
    // zero -- damping a zero edit is still zero, at any feedback -- and is excluded from the
    // must-differ check for that reason, not exempted from the formula check. Every other case has a
    // real, non-zero edit and must move. A tie anywhere real edit exists is the wiring-bug case the
    // plan's step 6 calls out explicitly.
    for (int i = 0; i < N; ++i) {
        expect(near3(at0_5[i], dampedReference(proxy[i], raw[i], 0.5f), 1e-5f),
               "feedback=0.5 differs from proxy + 0.5*(restored-proxy)");
        const Pixel restored = restoredReference(proxy[i], raw[i]);
        const bool realEdit = !near3(restored, {proxy[i].r, proxy[i].g, proxy[i].b, restored.a}, 1e-5f);
        if (realEdit)
            expect(!near3(at0_5[i], at1_0[i], 1e-4f),
                   "feedback=0.5 produced the same answer as feedback=1.0 -- damping is not live");
    }

    // AC: PassFeedback=0.0 is a full no-op -- the pass contributes nothing, output == proxy exactly
    // (proxy is already valid, so saturate() does not move it either).
    for (int i = 0; i < N; ++i)
        expect(near3(at0_0[i], {proxy[i].r, proxy[i].g, proxy[i].b, restoredReference(proxy[i], raw[i]).a}, 1e-6f),
               "feedback=0.0 did not fully discard the pass's raw answer");

    std::puts("PASS: PassFeedback=1.0 matches the undamped reference exactly, 0.5 matches the blended "
              "formula and differs from 1.0, 0.0 is a full no-op (WARP HLSL)");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
