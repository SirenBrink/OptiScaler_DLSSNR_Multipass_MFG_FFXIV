extern "C"
{
__declspec(dllexport) int __cdecl OptiScalerCompanion_BeginNamePlateV1(uint64_t session)
{
    FfxivCompanion::Status status;
    auto callback = FfxivCompanion::beginNativeDraw.load();
    if (!callback || !FfxivCompanion::mailbox.Query(session, status, FfxivCompanion::Now())) return 0;
    callback();
    return 1;
}
__declspec(dllexport) void __cdecl OptiScalerCompanion_EndNamePlateV1(uint64_t session)
{
    FfxivCompanion::Status status;
    auto callback = FfxivCompanion::endNativeDraw.load();
    if (callback && FfxivCompanion::mailbox.Query(session, status, FfxivCompanion::Now())) callback();
}
__declspec(dllexport) uint64_t __cdecl OptiScalerCompanion_OpenV1(uint32_t version, uint32_t frameSize,
    uint32_t plateSize, uint32_t statusSize, int64_t frequency)
{
    return FfxivCompanion::mailbox.Open(version, frameSize, plateSize, statusSize, frequency,
                                      FfxivCompanion::Frequency(), FfxivCompanion::Now());
}
__declspec(dllexport) int __cdecl OptiScalerCompanion_SubmitV1(uint64_t session,
    const FfxivCompanion::Frame* frame, const FfxivCompanion::Plate* plates, uint32_t bytes)
{
    if (!frame) return 0;
    return FfxivCompanion::mailbox.Submit(session, *frame, plates, bytes, FfxivCompanion::Now()) ? 1 : 0;
}
__declspec(dllexport) int __cdecl OptiScalerCompanion_QueryV1(uint64_t session,
    FfxivCompanion::Status* status, uint32_t bytes)
{
    if (!status || bytes != sizeof(*status)) return 0;
    return FfxivCompanion::mailbox.Query(session, *status, FfxivCompanion::Now()) ? 1 : 0;
}
__declspec(dllexport) void __cdecl OptiScalerCompanion_CloseV1(uint64_t session)
{
    FfxivCompanion::mailbox.Close(session);
}
}
