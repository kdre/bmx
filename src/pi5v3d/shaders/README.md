# Pi5 V3D Shader Blobs

This directory is reserved for readable shader sources, generated V3D 7.x QPU
blobs, and blob metadata used by the Pi5 V3D backend.

Runtime GLSL compilation is intentionally out of scope for BMX bare metal. Any
future committed binary blob must include:

- source/provenance
- input and output formats
- uniform layout
- texture layout assumptions
- required V3D version

The current `sharp` and `crt` paths consume the generated V3D 7.1 package in a
continuous fullscreen fragment renderer. `sharp` forces pass-through effect
uniforms; `crt` binds the live BMX scanline controls. `crt_soft` retains the
older CPU source-stage preview for diagnostics.

## Generated Mesa reference artifacts

`shader_artifact.h` defines the small C++ data model used for Mesa-derived
reference artifacts. Files under `generated/` are produced from
`shader_artifact.json` by:

```sh
tools/pi5/mesa-v3d-probe/generate_shader_header.py \
  --artifact build/host/mesa-v3d-probe/v3d-debug-clif/shader_artifact.json \
  --out src/pi5v3d/shaders/generated/scanline_probe_artifact.h \
  --symbol-prefix kMesaScanlineProbe \
  --name mesa-scanline-probe \
  --provenance 'Pi5 Mesa 24.2.8 V3D 7.1.10.2 scanline probe; generated from V3D_DEBUG=cl,clif,nir,vir,qpu,fs shader_artifact.json'
```

The legacy generated `scanline_probe_artifact.h` captures the Mesa scanline
probe's uniform templates, sampler template, CLIF data blocks, address-word
patches, and runtime patch points. Its embedded QPU arrays remain as historical
comparison data, but they are no longer the QPU source used by the renderer.

The production compiler workflow now lives in
`tools/v3d/offline-compiler/`. Shared GLSL and a pipeline manifest generate a
board-neutral package contract plus separate V3D 4.2 and V3D 7.1 headers under
`src/v3dcrt/shaders/generated/`. On Pi5,
`pi5_shader_package_adapter.*` validates the V3D 7.1 package and its exact
uniform/VPM requirements, then replaces the v1 fixture's stage-code pointers
with the desktop-generated v2 QPU arrays. Command lists, sampler words, and GPU
addresses remain Pi5 backend data. A schema, target, or capability mismatch
keeps the existing framebuffer fallback active.

Prepared packages are instance-owned rather than adapter singletons. Each
instance owns its dynamic stage-code metadata, copied uniform streams, patch
points, artifact, and runtime layout. The M6 source/output split uses this to
keep `crt_source_noise` and `crt_output_response` alive
concurrently. The smaller `crt_source_filter`, `crt_source_convergence`,
`crt_source_composite`, `crt_source_horizontal_jitter`, and
`crt_output_geometry` packages plus `crt_output_scanlines` and
`crt_output_edge_blur` plus `crt_output_phosphor_mask` and
`crt_output_vignette` plus `crt_output_uneven_illumination` and
`crt_output_glass_reflection` plus `crt_output_rounded_screen_mask` and
`crt_output_edge_glow` remain available as A/B references. The source
package applies three-tap Horizontal Filtering, channel-separated Convergence
in source-texel units, and Composite Artifacts from the already available
center/left/right samples, shifts all five sample coordinates from an explicit
source-row Horizontal Jitter wave, and adds Noise from integer source pixels.
The output package applies Edge Blur after Geometry, then evaluates a
source-row-phased beam and uses derivatives to integrate it over each output
pixel when Multisample is enabled. It finally applies the selected Phosphor
Mask pattern in physical output-pixel columns.
It therefore follows the warped picture instead of alternating HDMI rows. The
compatibility singleton API remains only for older host tests and diagnostic
wrappers; continuous multipass rendering always passes an explicit package
instance through its replay context.

`shader_artifact_materializer.*` is the first runtime-side step after header
generation. It copies artifact stage code, uniform templates, sampler templates,
and CLIF data-block templates into caller-provided runtime buffers, patches
explicit address words inside those data blocks, resolves patch points against a
runtime buffer binding table, and applies the patch points that already have a
concrete source word in the artifact. At this stage it does not encode or submit
V3D command lists; shader-state-record and primitive packet replay are still the
next missing pieces, while uniform, sampler, and shader-attribute address words
are patched immediately.

`v3dcrt_test=fragment_artifact` exercises that materializer on Pi5 during boot.
It maps the current Mesa scanline probe artifact into fixed slices of the
V3D-mapped control scratch buffer, logs the runtime V3D addresses, patched
uniform/sampler/attribute words, and all resolved patch points, then returns to
the normal boot path. It intentionally does not submit a draw until the GL shader
state and primitive packet replay path is implemented.

`v3dcrt_test=fragment_replay` is the hidden non-presenting diagnostic step. It
keeps the same materialized artifact, allocates a separate tile-allocation/TSDA
buffer, builds the small Mesa-derived CT0 binning list and CT1 render list for a
16x16 scratch target, submits both queues, logs binner/render completion state
and target samples, then returns to the normal boot path.

`v3dcrt_test=fragment_scanout` runs the same replay and presents the 16x16
rendered V3D target through direct Pi5KMS/HVS scanout, falling back to a CPU
scanout copy if the direct plane submit fails. This is still replay bring-up,
not the production CRT renderer.

`v3dcrt_test=fragment_fullscreen` reuses the same artifact and patches the
fragment, vertex, coordinate, clip, viewport, and tile geometry to the full
384x240 scratch target. It emits all required 64x64 supertile coordinates and
presents the result through the same direct Pi5KMS/HVS path. This validates
multi-tile fragment rendering before the fixed artifact is replaced with live
emulator texture input.

`v3dcrt_test=fragment_source` keeps that fullscreen replay but patches the
Mesa sampler address words to the BMX V3D source BO after staging a small RGBA
test texture there. This validates texture sampling from a BMX-owned source BO
before the fixed 4x4 texture is replaced with live emulator framebuffer input.

## Linux/Mesa exploration probe

`tools/pi5/mesa-v3d-probe/run_probe.sh` is the current Linux-side exploration
path. It builds a small host probe, creates a surfaceless EGL/OpenGL ES context,
compiles the GLSL ES sources in `mesa_probe/`, renders a tiny scanline test
image, and records whether Mesa exposes an OpenGL ES program binary for the
linked program. If a binary is available, the runner starts a second process,
loads it through `glProgramBinaryOES`, renders the same test image, and compares
the output checksum.

This is not a BMX runtime dependency. The original probe remains useful for
Linux exploration, while the controlled compiler in
`tools/v3d/offline-compiler/` now produces committed V3D 4.2 and V3D 7.1
headers. The exploratory probe's expected use is:

1. Run the probe on Raspberry Pi OS with a real Mesa V3D renderer.
2. Archive `summary.txt`, `probe.stdout`, `probe.stderr`, and any
   `program_binary.bin` artifact.
3. Compare exploratory output with the pinned machine-readable compiler export
   when diagnosing driver or hardware differences.

Any `program_binary.bin` produced by OpenGL ES must be treated as a driver
program-binary/cache object until proven otherwise. It is not assumed to be raw
QPU code or portable across Mesa versions.

Current Pi5 Mesa 24.2.8 observation:

- `GL_RENDERER=V3D 7.1.10.2`
- `glGetProgramBinaryOES` returns `GL_PROGRAM_BINARY_FORMAT_MESA` (`0x875f`)
- the binary reloads successfully in a second process and renders byte-identical
  output
- `V3D_DEBUG=nir,vir,qpu,fs` still emits NIR/VIR/QPU during the reload render

So the binary is useful as a Mesa cache/program object, while the `V3D_DEBUG`
QPU dumps are the better immediate reference for BMX-owned shader work.

## qpu-magic-store boot diagnostic

`v3dcrt_test=qpu` embeds a minimal V3D 7.x CSD/QPU diagnostic in the Pi5 backend.
It is not a CRT fragment shader. It exists only to prove that BMX can upload QPU
code, submit a compute shader dispatch, flush TMU/L2T writes, and verify a
V3D-written BO.

Uniform layout:

- word 0: V3D virtual address to write
- word 1: 32-bit magic value, currently `0x51375055` (`Q7PU`)

Readable source used to generate the embedded words:

```python
@qpu
def qpu_magic_store(asm):
    nop(sig=ldunifrf(rf0))
    nop(sig=ldunifrf(rf1))
    mov(tmud, rf1)
    mov(tmua, rf0)
    nop(sig=thrsw)
    nop(sig=thrsw)
    nop()
    nop()
    nop(sig=thrsw)
    nop()
    nop()
    nop()
```

The current embedded words were generated with a temporary checkout of
`Idein/py-videocore7` commit `4707e67`. The tool is not vendored here; the words
are kept local to the Pi5 backend until a project-owned shader toolchain exists.

Pi5/Pi500 hardware confirms this diagnostic: CSD reaches `CSDDONE`, reports no
hub/MMU faults, and the V3D-written target word matches `0x51375055`.

## qpu-fill boot diagnostic

`v3dcrt_test=qpu_fill` embeds a second minimal V3D 7.x CSD/QPU diagnostic. It
uses one QPU dispatch to fill the complete RGB565 target BO with a
duplicated RGB565 word, then BMX verifies multiple sampled 32-bit words and
copies the result into the Pi5KMS scanout backbuffer.

Uniform layout:

- word 0: number of 16-word groups to write
- word 1: V3D virtual output address
- word 2: duplicated RGB565 word, currently `0x07e007e0`

Readable source used to generate the embedded words:

```python
@qpu
def qpu_fill_rgb565_words(asm):
    nop(sig=ldunifrf(rf11))
    nop(sig=ldunifrf(rf12))
    nop(sig=ldunifrf(rf13))

    eidx(rf10)
    shl(rf10, rf10, 2)
    add(rf12, rf12, rf10)

    mov(rf10, 4)
    shl(rf10, rf10, 4)

    with loop as l:
        sub(rf11, rf11, 1, cond="pushz").mov(tmud, rf13)
        l.b(cond="anyna")
        mov(tmua, rf12)
        nop()
        tmuwt().add(rf12, rf12, rf10)

    nop(sig=thrsw)
    nop(sig=thrsw)
    nop()
    nop()
    nop(sig=thrsw)
    nop()
    nop()
    nop()
```

The current embedded words were generated with the same temporary
`Idein/py-videocore7` commit `4707e67` checkout used for the magic-store
diagnostic.

Pi5/Pi500 hardware confirms this diagnostic: CSD reaches `CSDDONE`, reports no
hub/MMU faults, and the first/middle/last sampled target words all match
`0x07e007e0`.

## frame-copy runtime diagnostic

`v3dcrt_shader=frame_copy` is the first runtime QPU frame path. It is not yet a
CRT shader. The CPU still stages the emulator source into the RGB565 source BO;
then a V3D 7.x CSD/QPU program copies a 384x240 region from the source BO into
a double-buffered target BO, preserving source and target row pitch. BMX
verifies sampled target pixels on the first frame and presents completed
targets directly through Pi5KMS/HVS when possible, with CPU scanout copy
fallback. Direct HVS scanout uses the existing interpolation setting to choose
nearest or Mitchell scaling.

Uniform layout:

- word 0: rows to copy, currently `240`
- word 1: 16-word groups per row, currently `12`
- word 2: source V3D virtual address
- word 3: target V3D virtual address
- word 4: source row skip after copied groups
- word 5: target row skip after copied groups

Readable source used to generate the embedded words:

```python
@qpu
def qpu_copy_rgb565_rows(asm):
    nop(sig=ldunifrf(rf20))
    nop(sig=ldunifrf(rf21))
    nop(sig=ldunifrf(rf2))
    nop(sig=ldunifrf(rf3))
    nop(sig=ldunifrf(rf4))
    nop(sig=ldunifrf(rf5))

    eidx(rf10)
    shl(rf10, rf10, 2)
    add(rf2, rf2, rf10)
    add(rf3, rf3, rf10)

    mov(rf13, 4)
    shl(rf13, rf13, 4)

    with loop as row_loop:
        mov(rf22, rf21)
        with loop as group_loop:
            mov(tmua, rf2, sig=thrsw).add(rf2, rf2, rf13)
            nop()
            nop()
            nop(sig=ldtmu(rf10))
            mov(tmud, rf10)
            sub(rf22, rf22, 1, cond="pushz").mov(tmua, rf3)
            group_loop.b(cond="anyna")
            tmuwt().add(rf3, rf3, rf13)
            nop()
            nop()

        add(rf2, rf2, rf4)
        add(rf3, rf3, rf5)
        sub(rf20, rf20, 1, cond="pushz")
        row_loop.b(cond="anyna")
        nop()
        nop()
        nop()

    nop(sig=thrsw)
    nop(sig=thrsw)
    nop()
    nop()
    nop(sig=thrsw)
    nop()
    nop()
    nop()
```

The current embedded words were generated with the same temporary
`Idein/py-videocore7` commit `4707e67` checkout used for the boot diagnostics.

## scanlines runtime effect

`v3dcrt_shader=scanlines` uses the same source staging, double-buffered target
BOs, CSD submit, first-frame verification, and direct HVS scanout as
`frame_copy`, but replaces the QPU row copy program with a first visible effect
program. It alternates normal copied rows and darker RGB565 rows. The runtime
job computes an effective dark-row brightness from `scanline_gap_brightness`
and a coarse `scanline_weight` approximation, quantizes it to sixteenths, and
passes four RGB565 term masks. The QPU computes dark rows as a sum of optional
1/2, 1/4, 1/8, and 1/16 terms. The framebuffer layer starts with a half-bright
gap default before menu values arrive, but menu-provided values, including gap
brightness 0.0, are used directly. For this diagnostic path,
`scanline_weight` maps 0.0..15.0 to a blend strength toward
`scanline_gap_brightness`: 0.0 leaves rows unchanged, 15.0 applies the full gap
brightness, and intermediate values are quantized to sixteenths. Only explicit
no-op controls, currently weight 0.0 or gap brightness 1.0, select the runtime
QPU copy path; rounded 16/16 values still use the scanline row program and are
clamped to a visible 15/16 row scale.
This is still a diagnostic runtime effect, not the final CRT shader.

Uniform layout:

- word 0: rows to copy from the runtime frame job
- word 1: 16-word groups per row from the runtime frame job
- word 2: source V3D virtual address
- word 3: target V3D virtual address
- word 4: source row skip after copied groups
- word 5: target row skip after copied groups
- word 6: duplicated RGB565 1/2 term mask from the runtime frame job
- word 7: duplicated RGB565 1/4 term mask from the runtime frame job
- word 8: duplicated RGB565 1/8 term mask from the runtime frame job
- word 9: duplicated RGB565 1/16 term mask from the runtime frame job

Readable source used to generate the embedded words:

```python
@qpu
def qpu_scanline_rgb565_rows(asm):
    nop(sig=ldunifrf(rf20))
    nop(sig=ldunifrf(rf21))
    nop(sig=ldunifrf(rf2))
    nop(sig=ldunifrf(rf3))
    nop(sig=ldunifrf(rf4))
    nop(sig=ldunifrf(rf5))
    nop(sig=ldunifrf(rf6))
    nop(sig=ldunifrf(rf7))
    nop(sig=ldunifrf(rf8))
    nop(sig=ldunifrf(rf9))

    eidx(rf10)
    shl(rf10, rf10, 2)
    add(rf2, rf2, rf10)
    add(rf3, rf3, rf10)

    mov(rf13, 4)
    shl(rf13, rf13, 4)
    mov(rf23, 0)

    def emit_group_loop(darken):
        mov(rf22, rf21)
        with loop as group_loop:
            mov(tmua, rf2, sig=thrsw).add(rf2, rf2, rf13)
            nop()
            nop()
            nop(sig=ldtmu(rf10))
            if darken:
                shr(rf11, rf10, 1)
                band(rf11, rf11, rf6)
                shr(rf12, rf10, 2)
                band(rf12, rf12, rf7)
                add(rf11, rf11, rf12)
                shr(rf12, rf10, 3)
                band(rf12, rf12, rf8)
                add(rf11, rf11, rf12)
                shr(rf12, rf10, 4)
                band(rf12, rf12, rf9)
                add(rf10, rf11, rf12)
            mov(tmud, rf10)
            sub(rf22, rf22, 1, cond="pushz").mov(tmua, rf3)
            group_loop.b(cond="anyna")
            tmuwt().add(rf3, rf3, rf13)
            nop()
            nop()

    with loop as row_loop:
        band(null, rf23, 1, cond="pushz")
        b(R.dark_row, cond="allna")
        nop()
        nop()
        nop()

        emit_group_loop(False)
        b(R.after_row, cond="always")
        nop()
        nop()
        nop()

        L.dark_row
        emit_group_loop(True)

        L.after_row
        bxor(rf23, rf23, 1)
        add(rf2, rf2, rf4)
        add(rf3, rf3, rf5)
        sub(rf20, rf20, 1, cond="pushz")
        row_loop.b(cond="anyna")
        nop()
        nop()
        nop()

    nop(sig=thrsw)
    nop(sig=thrsw)
    nop()
    nop()
    nop(sig=thrsw)
    nop()
    nop()
    nop()
```

## fragment_probe runtime diagnostic

`v3dcrt_shader=fragment_probe` is the first guarded runtime use of the
Mesa-derived fragment replay artifact. It is not a general fragment shader blob
format yet. The Pi5 backend prepares the Mesa-derived artifact, shader state,
sampler patch, and fullscreen BCL/RCL during V3D initialization. At frame time
it stages the current emulator source into the RGB565 source BO, waits until a
4x4 grid sampled from that live frame contains at least one non-black pixel,
copies that sample into the already validated RGBA texture layout, submits the
prepared fullscreen CT0/CT1 replay once, and presents the V3D target through
Pi5KMS/HVS when possible.

This path intentionally keeps the known-good 4x4 sampler descriptor rather than
guessing the full 384x240 texture descriptor fields. If early boot frames are
still black, the probe defers for a bounded number of frames and keeps using
the existing framebuffer present path. After the one-shot probe, BMX falls back
to that path as well.

The runtime path logs a `fragment replay timings` line for the prepared submit.
The fields split the one-shot frame cost into CT0 binning, CT1 render,
target-buffer invalidate, target sample scan, direct HVS present, and fallback
present phases.
Successful runtime BCL/RCL and direct-present register dumps are suppressed for
that timing line so UART output does not dominate the measured phase durations.
The normal prepared runtime probe also skips the full target-buffer sample scan
and the live-source word dump; the separate boot-test path keeps those verbose
diagnostics.
For present-cost isolation, `v3dcrt_fragment_probe_wait_vblank=0` disables only
the runtime `fragment_probe` Direct-Present VBlank wait. The default remains
`1`, matching the normal frame-present path.

## sharp continuous fragment path

`v3dcrt_shader=sharp` is the first continuous consumer of the generated V3D
7.1 fragment package. Indexed8 input is expanded through the live RGB565
palette and RGB565 input is converted directly into an RGBA8 source texture.
The CPU stager writes the V3D-selected LINEARTILE, UBLINEAR, or UIF layout;
384x240 RGBA8 uses UIF-XOR with 384x256 padded storage. The matching
`pi5_v3d71_texture_state.*` code packs the board-specific texture descriptor
and nearest/linear sampler state. Native tests check the 4x4 descriptor
byte-for-byte against the known Mesa capture and verify complete,
non-overlapping address coverage for the 384x240 UIF-XOR layout. The continuous
BMX live-frame descriptor additionally sets `flip_texture_y_axis` to bridge
BMX's top-down source rows to the captured Mesa/GL bottom-left texture
coordinates; diagnostic descriptors retain their captured orientation.

For `sharp`, the scanline package is deliberately used as a pass-through by
forcing scanline weight to `0.0`. The fragment pass runs once per emulator
frame and alternates between two RGB565 V3D targets.
Pi5KMS either scans out that target directly or combines its returned HVS plane
with menu/status overlays. Descriptor, submit, or present failures permanently
disable this runtime path for the boot and leave the existing framebuffer
renderer available.

This establishes full live-frame texture sampling and continuous CT0/CT1
submission.

The experimental M6 output-resolution probe is selected with
`v3dcrt_render_resolution=output` together with `v3dcrt_shader=sharp`.
It uses the smaller generated `scanline_probe` package with scanline weight
forced to zero, allocates two RGB565 targets matching the visible destination
rectangle, and presents that target 1:1 through HVS. The default
`v3dcrt_render_resolution=source` retains the established 384x240 target.
Other shader presets normally fall back to source resolution even when
`output` is requested. The `crt` preset automatically selects the
effect-bearing source/output split. The historical `core` package override is
accepted as a Debug-build compatibility alias but is no longer required.

The output probe accepts targets up to 1920x1080. Target pitch, tile-allocation
memory, and the 256-byte-per-tile state array are derived from the requested
size with checked arithmetic. Allocation or preparation failure logs one
`output-resolution probe fallback` line and retries the established 384x240
fragment path; a failure there still leaves the existing non-V3D framebuffer
fallback available. Pi5/Pi500 hardware confirms the 1152x720 direct-HVS probe
at C64 PAL 720p50 without V3D/MMU failures or VSync warnings. The subsequent
M6 hardware matrix validated the output-space effect semantics and frame budget
for the supported 720p modes.

The production path uses `v3dcrt_shader=crt` and
`v3dcrt_render_resolution=output`. It exercises the GPU-only source/output
effect split without a fragment-package override. Before the GPU passes, the
shared Pi4/Pi5 Output Response source stage transforms the complete emulator
viewport. Indexed8 rewrites a cached 256-entry palette; RGB565 lazily builds a
65,536-entry LUT only while the effect is enabled. Accurate uses the configured
input/output-gamma ratio, while Fast intentionally performs no gamma power
operation. A source pass then uses the
dedicated `crt_source_noise` package, applies the explicit three-tap
Horizontal Filtering, five-sample channel-separated Convergence, and Composite
Artifacts plus source-row Horizontal Jitter and source-pixel Noise at 384x240,
and writes an RGBA8
UIF-XOR intermediate target. The earlier `crt_source_filter`,
`crt_source_convergence`, `crt_source_composite`, and
`crt_source_horizontal_jitter` packages remain A/B
references. Composite reuses center/left/right, and Jitter shifts those five
coordinates without adding a texture read. Noise is added after Composite from
integer source-pixel coordinates and also adds no texture read. Its arithmetic
hash cross-mixes X, Y, and an independent frame seed; luminance, chroma I,
chroma Q, and the intentional row component use separate seeds. Animation
therefore changes the field rather than translating a periodic spatial
pattern. A second context owns
`crt_output_response`,
samples that target through the TMU, and applies Geometry, Edge Blur, a
source-row-phased beam, Phosphor Mask, Vignette, Uneven Illumination, and Glass
Reflection, followed by Edge Glow and Rounded Screen Mask,
while writing the destination-sized RGB565 scanout target. The narrower
`crt_output_geometry`,
`crt_output_scanlines`, `crt_output_edge_blur`, and
`crt_output_phosphor_mask` plus `crt_output_vignette` and
`crt_output_uneven_illumination` plus `crt_output_glass_reflection` and
`crt_output_rounded_screen_mask` plus `crt_output_edge_glow` packages remain
A/B references. Edge Blur
reads the warped center and four neighbors in
source-texel units, but computes its focus-loss mask per destination pixel from
the post-Geometry source coordinate. The beam is evaluated
in output space and derivative-integrated when Multisample is enabled, so it
follows warped emulator rows instead of alternating physical output rows. The
BMX Interpolation option controls that second sampler and remains independent
of the source-domain filter. The source sampler stays nearest while Convergence
and Jitter are both off and switches to linear while either is on; this gives
fractional source-texel displacement continuous coverage without changing the
five-sample TMU contract. Composite itself does not change the source sampler
policy, and Noise does not change it. Phosphor Mask adds no texture access and
uses `gl_FragCoord.x` only after output-space Geometry, Edge Blur, and Beam.
Vignette, Uneven Illumination, Glass Reflection, and Rounded Screen Mask add no
texture access. Edge Glow receives four temporally filtered RGB uniforms
derived from narrow top, bottom, left, and right source-edge regions. The Pi5
runtime averages a weighted 3x3 strip per edge, for 36 CPU-side samples per
frame. The output shader assigns each color only to its adjacent edge and
normalizes the two contributions in corners. It has no center sample or global
floor and keeps the output pass at five TMU samples.
Vignette attenuates the masked result radially; Illumination applies four
low-frequency fields from the same normalized output coordinates; Reflection
adds an angled stripe and weak radial Fresnel component; Rounded Screen Mask
applies signed-distance coverage after the warm Edge Glow, preventing glow
outside the clipped screen. All five are independent of source resolution. The
V3D store packet and offline fixed-function contract both keep hardware
dithering disabled. The output shader therefore applies one stable Bayer 4x4
threshold in output coordinates after the continuous modulations and mask,
using the larger continuous deviation and the RGB565 channel quanta. Glass
Reflection contributes its full effect amplitude and Edge Glow its
configured strength instead of fading the dither with their local soft fields.
Pixels with zero mask coverage bypass dither and remain exactly black. The two
passes
have separate artifact, command-list, tile-allocation, and TSDA storage; no CPU
copy or repack occurs between them. Output Response has already run on the
source image, so the cumulative output package binds its response switch off.
Levels remap Rec.709 luminance and restore gamut-safe chroma at Saturation
`0..1`; they do not clip RGB channels independently. Level Mapping selects
direct hard thresholds (`Linear`), cubically remapped hard thresholds
(`Cubic`), or a smooth fourth-power shadow toe/highlight shoulder
(`Toe / Shoulder`). Disabling the group preserves the source bytes. Bloom Off or factor
zero keeps this two-pass path unchanged. Bloom On adds three output-domain
passes: `crt_output_response` first writes the full-size base image to tiled
RGBA8, `crt_bloom_blur_horizontal` extracts bright energy while reducing only
the width to one quarter, `crt_bloom_blur_vertical` applies the anti-aliased
vertical reduction to quarter height, and `crt_bloom_composite` samples both
base and blur textures before writing the normal RGB565 scanout target. Rounded
Screen Mask moves from the base pass to the final composite while Bloom is
active so it is not multiplied twice.
`crt_output_response_fast_cubic` remains a compiler and shader-path A/B
specialization, but production no longer selects it: Fast+Cubic is handled in
the common source stage and the generic cumulative output package runs with
Response disabled.
UART reports
`output_scope=source-output-split`, a changing `fragment source-domain state`,
both pass effects and timings, and the intermediate layout. Allocation,
descriptor, or submit failure leaves the source-resolution and framebuffer
fallbacks available. Pi5 hardware validation of the preceding effect-free
handoff confirmed correct orientation and channel order, live Geometry Off/On,
and nearest/linear switching. Stable capture windows remained at approximately
49.18 fps without V3D/MMU failures, render fallbacks, or VSync-behind warnings.
A subsequent Pi5/Pi500 test confirmed the Horizontal Filtering assignment
visually across Sigma X `0..100`, repeated filter Off/On, Geometry, and
independent Nearest/Linear interpolation. UART kept the output pass at
`horizontal_filter=off`, and no V3D/MMU failure, render fallback, timeout, or
VSync-behind warning occurred. The cumulative Composite revision is compiler-
and host-validated for V3D 4.2 and V3D 7.1. Pi5/Pi500 bare-metal testing also
confirms Composite Off/On, all three parameter-boundary checks, and the full
preceding source/output regression set without a reported V3D/MMU failure,
render fallback, timeout, or VSync-behind warning.
The cumulative `crt_output_edge_blur` package is compiler-validated for V3D
4.2 and hardware-compiled and drawn on Pi5 V3D 7.1. Its fragments contain
210/172 QPU words and 43 uniform words, use five TMUs and four threads, and do
not spill. The Pi5 draw checksum is `0x268ba8ad`. Pi5/Pi500 bare-metal visual
testing confirms the Off baseline, On/Off, Strength/Radius ranges, the curved
Geometry edge, and the preceding visual regressions. This run did not include
a fresh UART capture and therefore adds no new timing or fault-diagnostic
claim.
The cumulative `crt_output_phosphor_mask` package retains that complete output
path and adds the Green/Magenta and Trinitron/RGB column patterns plus live Mask
Brightness. Its V3D 4.2/7.1 fragments contain 239/197 QPU words and 49 uniform
words, use five TMUs and four threads, and do not spill. The real Pi5 V3D 7.1
draw succeeds with checksum `0x473fcecd`. The requested Pi5 bare-metal visual,
parameter, and cumulative regression matrix was subsequently confirmed as
passed. No fresh UART capture accompanied that confirmation, so it adds no
separate timing or fault-diagnostic claim.
The cumulative `crt_output_vignette` package retains that complete output path
and applies Strength, Scale, and Softness after Phosphor Mask in normalized
output coordinates, followed by shader-owned RGB565 ordered dithering in the
attenuated region. Its V3D 4.2/7.1 fragments contain 299/250 QPU words and 59
uniform words, use five TMUs and four threads, and do not spill. The real Pi5
V3D 7.1 draw succeeds with checksum `0xe6dc43da`. An initial bare-metal run
confirmed the parameters but exposed banding before the dither revision;
the subsequent Pi5 visual test confirmed that the ordered dither removes the
hard transitions without a reported 4x4 grid or temporal flicker. No fresh
UART capture accompanied that confirmation, so it adds no separate timing or
fault-diagnostic claim.
The cumulative `crt_output_uneven_illumination` package retains the Vignette
path and adds the Strength/Scale-controlled output-space field. Its V3D
4.2/7.1 fragments contain 379/319 QPU words and 84/80 uniform words, use five
TMUs and four threads, and do not spill. The real Pi5 V3D 7.1 draw succeeds
with checksum `0x46aaac15`. The requested Pi5 bare-metal visual and cumulative
regression test was subsequently confirmed as passed. No fresh UART capture
accompanied that confirmation, so it adds no separate timing or
fault-diagnostic claim.
The cumulative `crt_output_glass_reflection` package retains that complete
output path and adds the Angle/Width/Position-controlled reflection stripe and
weak radial Fresnel component. The shared ordered RGB565 dither is applied
once after Vignette, Uneven Illumination, and Reflection; Reflection contributes
its full amplitude across the complete soft field. Its V3D 4.2/7.1
fragments contain 440/368 QPU words and 98/94 uniform words, use five TMUs and
four threads, and do not spill. The real Pi5 V3D 7.1 draw succeeds with
checksum `0xdd7b11a3`. Pi5 bare-metal visual, parameter, timing, and cumulative
regression validation was subsequently confirmed as passed. No fresh UART
capture accompanied that confirmation, so it adds no separate timing or
fault-diagnostic claim.
The cumulative `crt_output_rounded_screen_mask` package retains that complete
output path and multiplies the finished output by the Corner Radius/Border
Softness-controlled signed-distance coverage. Dither is suppressed where
coverage is zero, leaving the exterior exactly black. Its V3D 4.2/7.1
fragments contain 484/407 QPU words and 105/100 uniform words, use five TMUs
and four threads, and do not spill. A deliberately maximal 16x16 probe reaches
pixel centers and the real Pi5 V3D 7.1 draw produces the distinct checksum
`0x18325bdd`. Pi5 bare-metal visual, parameter, and cumulative regression
validation was subsequently confirmed as passed. No fresh UART capture
accompanied that confirmation, so it adds no separate timing or
fault-diagnostic claim.
The cumulative `crt_output_edge_glow` package retains Rounded Screen Mask and
adds the Strength/Width-controlled warm edge-local field before rounded
coverage. It consumes the same top, bottom, left, and right frame uniforms as
the production package, so no Edge-Glow texture access remains in the output
shader. The final RGB565 dither follows configured Glow strength uniformly
rather than the already-faded local field, while zero mask coverage remains
exactly black. Its V3D 4.2/7.1 fragments contain 574/479 QPU
words and 127/123 uniform entries, use five TMUs and four threads, and do not
spill. The real Pi5 V3D 7.1 draw succeeds with checksum `0xce3d8510`.
Strength and Width retain their square-root response. Executed host-GLES
regressions verify monotonic controls, reject center-detail copies, prove that
a bright center behind a wide black frame does not light the outer edge, and
verify that one bright source edge affects only its corresponding output side.
The dedicated Bloom multipass remains responsible for local light spill into
adjacent dark pixels.
The cumulative `crt_output_response` package retains Edge Glow and the
generated Enable/Fast switches plus Input Gamma, Output Gamma, Saturation,
Black Level, White Clip, and Level Mapping as an offline/A/B reference. The
production runtime applies the board-neutral CPU transform to the complete
source viewport and binds this shader response disabled. It has 678/573 V3D
4.2/7.1 fragment QPU words and
146/142 uniform entries, uses five TMUs and four threads, and does not spill. The V3D 4.2
desktop profile compiles deterministically; the real Pi5 V3D 7.1 driver
compiles and draws the package with checksum `0xa838552b`. Ten Output-Response
tests cover disabled invariance, both transfer modes, levels, saturation, the
placement around Scanlines, all three mappings, and the exact C64 light-blue
narrow-level
regression. General Pi5 bare-metal rendering and menu plumbing are confirmed;
bare-metal validation also confirms that the corrected level math no longer
inverts hues. The menu now persists a board-neutral Level Mapping enum:
`Linear`, `Cubic`, or `Toe / Shoulder`; `Cubic` remains the default. The Pi5
backend patches the enum and normalized controls without redefining them.
Live switching among the three modes remains visually unvalidated on bare
metal.
The Bloom chain adds `crt_bloom_blur_horizontal`,
`crt_bloom_blur_vertical`, and `crt_bloom_composite`. Their V3D 4.2/7.1
fragment programs contain 72/59, 99/76, and 65/60 QPU instructions. The blur
passes use three and eleven TMU requests respectively; the composite uses two
independent sampler semantics. All use four threads and no spill. The offline
probe accepts one to four contiguous
sampler units, and the Pi5 adapter strips Mesa's encoded unit byte before
rebasing each semantic onto its runtime texture descriptor and sampler state.
The three V3D 7.1 packages compile and execute on Linux renderer
`V3D 7.1.10.2` without a GL error. Host tests cover the full-height horizontal
intermediate, quarter-size final blur, package contracts, independent TMU
address patching, factor-zero identity, positive light addition, and
rounded-boundary clipping. The GLES regression executes the complete
blur/composite chain with separate full-size, quarter-width/full-height, and
quarter-size textures. It reproduces the first bare-metal finding: the former
`0.45..0.85` luminance extraction rejected C64 light blue and therefore made
Factor inert on the start screen. The corrected soft interval is `0.20..0.65`;
C64 dark blue stays below it while light blue contributes continuously. This
chain passes on host GLES and real Pi5 V3D. Pi5 bare metal visually confirms
that Bloom and Factor now affect the C64 start screen; a separate timing,
VSync, and fallback-diagnostic run remains outstanding.

The Bloom reduction keeps thresholding at full output height to avoid folding
scanline energy into a low-frequency envelope. The horizontal kernel uses the
moment-matched distribution `0.2, 0.6, 0.2` at offsets
`-sqrt(10), 0, +sqrt(10)`. The vertical pass reduces directly to quarter height
with a contiguous 22-tap variance-16 FIR collapsed into eleven bilinear TMU
requests; snapping its source anchor to the nearest texel boundary also keeps
non-multiple-of-four heights phase-stable. Pi5 Linux validation covered all 40
unique source/output size tuples in `sdcard/machines.ini`: the worst difference
from a dense 33-tap reference was one channel step and the worst additional
spectral peak was 0.234 percent. Isolated filter-pair timing increased by
0.323 ms at 720p and 0.736 ms at 1080p versus the aliased path. Bare-metal
x64sc validation of this revision is still pending.
The cumulative `crt_source_horizontal_jitter` package is compiler-validated
for V3D 4.2 and hardware-compiled and drawn on Pi5 V3D 7.1. Its fragments
contain 183/158 QPU words and 47 uniform words, use five TMUs and four threads,
and do not spill. The Pi5 draw checksum is `0x9817d8c1`. The requested Pi5
bare-metal visual and parameter matrix, sampler transition, diagnostics, and
cumulative source/output-effect regressions were subsequently confirmed as
passed; no separate fresh UART excerpt accompanied that confirmation.

The cumulative `crt_source_noise` package is compiler-validated for V3D 4.2
and V3D 7.1. Its fragments contain 293/254 QPU words and 82 uniform words, use
five TMUs and four threads, and do not spill. Noise is generated from integer
source pixels after Composite and does not alter the Convergence/Jitter sampler
policy. A frame uniform seeds the cross-mixed hash independently; Speed zero is
static, while nonzero Speed selects new deterministic fields without moving a
fixed lattice across the picture. Distribution tests cover mean, variance,
frame/color-seed correlation, and the old `(2,13)` diagonal recurrence. The
current bare-metal Pi400/Pi500 C64 captures are byte-identical at Speed zero,
all three Speed-50 captures change, and both targets retain 50 FPS.

The `core` package option remains the temporary feature gate for this guarded
path, but it no longer means that `crt_core_probe` executes in either pass.
UART logs both actual package roles and IDs. Host tests validate concurrent
package ownership and both generated hardware generations. Pi5/Pi500 hardware
confirms both package roles, Horizontal Filtering and Geometry without
V3D/MMU faults, render fallback, timeout, or VSync-behind warning at stable
approximately 49.18 fps. A subsequent Pi5/Pi500 run confirms
`crt_output_scanlines` visually across Scanlines On/Off, Weight and Gap
Brightness sweeps, Multisample, Geometry, and Nearest/Linear interpolation.
The capture starts after package preparation but records the active split and
runtime uniform changes. Stable windows return to approximately 49.18 fps
without V3D/MMU faults, render fallback, timeout, or VSync-behind warning.
The next source-domain gate adds Convergence only. V3D 4.2 and 7.1 package
generation and host contract tests pass with five TMU samples and no spill;
Pi5/Pi500 bare-metal validation confirms continuous Red/Blue X/Y and radial
controls, nearest sampling while Convergence is off, linear sampling while it
is on, and stable approximately 49.18 fps windows without V3D/MMU faults,
fragment-render fallbacks, timeouts, or VSync-behind warnings.

All generated CRT shader sources share the subsequent Geometry refinement.
Horizontal curvature scales X from Y squared, vertical curvature scales Y from
X squared, and overscan is applied after both. This avoids the old
aspect-weighted radial compression. A `GL_OES_standard_derivatives` coverage
calculation replaces the binary source-boundary test with an approximately
one-output-pixel transition. The texture sampler remains independently
nearest/linear; edge coverage does not blur interior source pixels. Pi5
hardware confirmed smooth edges without visible stair steps. Both menu controls
now expose `0..100`, mapped to an internal `0..1/6` range.

## crt continuous scanline path

`v3dcrt_shader=crt` uses the same continuous texture, geometry, double-target,
and Pi5KMS/HVS path as `sharp`, but enables the generated fragment shader's
scanline modulation. Runtime code finds the generated uniform-stream entries by
their package semantics: `fragcoord_y_scale`, `fragcoord_y_bias`,
`scanline_gap_brightness`, and `scanline_weight`. This keeps generated package
ordering out of the BMX effect interface while the Pi5 adapter remains free to
validate its exact V3D 7.1 contract.

The BMX scanline Weight range `0..15` maps linearly to fragment strength
`0..1`; Gap Brightness remains `0..1`. Disabling Scanlines or setting Weight to
zero produces the same pass-through as `sharp`. A Gap Brightness of `1.0` also
has no visible modulation. These mappings establish predictable controls, not
final CRT quality. At that milestone, Gamma, mask, horizontal filtering,
geometry, Bloom, and Output Response still needed generated shader work; the
later cumulative and multipass packages described above supersede that state.

Pi5/Pi500 hardware validates this path at C64 PAL 720p50. Scanlines On/Off and
the full Weight and Gap Brightness ranges update live, including pass-through
at Weight zero and Gap Brightness one. The test completed without V3D/MMU
faults, runtime renderer fallback, or VSync-behind warnings and returned to a
steady 50 fps after menu interaction.
