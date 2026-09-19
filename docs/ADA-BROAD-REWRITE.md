# Ada MFG broad kernel rewrite test

The Ada unlock now uses the broad Blackwell PTX retargeting implementation adapted from ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass, revision db6cc0e, OptiScaler/framegen/dlssg/MfgUnlock.cpp (GPL-3.0; originally adapted from y4my4my4m).

Compatible containers retarget SM120 PTX to SM89, update its target directive without changing payload length, and park existing SM89 images at SM122. This replaces the targeted temporal correction. The existing Ada-only eligibility gate, provider tracking, module retention, and capability-gate checks remain. Local adaptations journal original containers for rollback and reject multiple candidate Blackwell PTX images in one container.

Enable the existing Ada MFG unlock option and restart. There is no additional compatibility DLL for this RTX40 path. ShyVortex's SM75/SM86 loader is a separate RTX20/30 integration and is not included here. The NR runtime is independent.

Validation: Release x64 build; extracted production rewrite tests covering two containers, image parking, repeat application, exact restoration, simulated write failure, ambiguous PTX, and malformed sizes. A non-executing mapping of the local nvngx_dlssg.dll yielded 31 rewritten containers and exact rollback. Disk SHA256 remained FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82. These checks do not establish GPU execution correctness or long-session stability.

RTX40 testing pending: compare the same GPU, driver, runtimes, INI and 3X factor with the previous build. Save logs after each run. Keep the patched NR runtime unchanged. Do not combine this build with another Ada MFG unlocker.
