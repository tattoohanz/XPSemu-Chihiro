# Patches to PS5_Vulkan's RADV (not part of a normal build)

`ps5/build-title.sh` links RADV from PS5_Vulkan (`RADV_ARCHIVE`, default
`.deps/native/radv-release/lib/libvulkan_radeon.ps5.a`). A patch here is
applied to PS5_Vulkan's staged Mesa tree (`.deps/work/radv-src`), the
archive rebuilt with
`ninja -C .deps/work/radv-build-ps5-release src/amd/vulkan/libvulkan_radeon.a`,
copied to a file named `libvulkan_radeon.ps5.a` and passed to the title
build as `RADV_ARCHIVE=<that file>`.

- `radv-slow-submit-diag.patch` (log only): any queue submission over 40 ms
  logs where it went (waits, submit lock, ring, copy, AGC), as do a slow
  `sceAgcDriverSubmitDcb`, `sceAgcSuspendPoint` and `sceVideoOutSubmitFlip`.
  For the ~110 ms submission stalls about every 10 s (OutRun 2 SP, Ghost
  Squad; d259035's game logs).
