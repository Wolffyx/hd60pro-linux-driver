# MZ0380 SDK Control-Path Notes

This note records the minimum control-path evidence mined from the local SDK
before any BAR writes, mailbox experiments, or DMA work.

The current driver-side handoff for this note is `/proc/mz0380-control`, which
reuses the existing safe BAR5 snapshot reads and groups them in the same
SDK-derived control order.

## Scope
- SDK root used for mining:
  - `/home/wolffyx/Downloads/test/SDK 1.1.0.202.0/RELEASES 1.1.0.202.0`
- Primary sample and document anchors:
  - `AMESDK/SAMPLES/OTHERS/SCHEDULE.RECORDING/ScheduleRecording/SetupDialog.cpp`
  - `AMESDK/SAMPLES/OTHERS/SCHEDULE.RECORDING/ScheduleRecording/res/MZ0380.txt`
  - `AMESDK/SAMPLES/OTHERS/SCHEDULE.RECORDING/ScheduleRecording/FileRenderer.cpp`
  - `AMESDK/SAMPLES/PRODUCTS/SC540.PRODUCTS/SC540.NET.VB/SC540/Form1.vb`
  - `AMESDK/SAMPLES/PRODUCTS/SC540.PRODUCTS/SC540.AAC.NET.C#/SC540/MyPeopertyDlg.cs`
  - `AMESDK/INC/AMESDK.H`
  - `DSHOW/DOC/Yuan's Device Custom Property Table 1.1.0.164.8.pdf`
  - `DSHOW/DOC/Yuan's SC540 DirectShow Software Programming Guide 1.1.0.161.0.pdf`

## High-Value Source Anchors
- `SetupDialog.cpp`
  - `1378..1382`: `MZ0380 PCI` selects the `TEXT_0380` resource blob.
  - `1444..1480`: the parser loads NTSC or PAL resolution, frame rate, record mode,
    bitrate, quality, and B-frame lists from that text profile.
  - `1491..1493`: substream and B-frame capability flags are derived from the profile.
- `res/MZ0380.txt`
  - `1..13`: the full MZ0380 profile is SC540-shaped and includes only `RESOLUTION.MAIN`,
    not `RESOLUTION.SUB`.
- `FileRenderer.cpp`
  - `674..678`: create `MZ0380 PCI` raw and `MZ0380 PCI, Analog Encoder` handles.
  - `688..705`: configure the encoder handle with standard, H.264 format,
    deinterlace, record mode, quality, bitrate, GOP, and B-frames.
  - `783..850`: create and configure `MZ0380 PCI, Analog WaveIn`.
  - `901..917`: configure the raw handle and call `RUN` on the active handles.
- `Form1.vb`
  - `595..667`: create raw handles, set standard and YV12 format, then query
    serial and format state.
  - `673..729`: create encoder handles and push the compression-property sequence.
  - `737..761`: create and configure audio handles.
  - `767..795`: fetch serial via custom property `0`, then call `RUN`.
- `MyPeopertyDlg.cs`
  - `27`: read input source with custom property `201`.
  - `57, 62, 67, 72, 77`: write input selections `0..4` through property `201`.
- `AMESDK.H`
  - `513..544`: generic `GET/SET_VIDEOCOMPRESSION_PROPERTY` interface and supported
    generic property indices.

## Device Model Exposed By The SDK
- `MZ0380 PCI` is handled as an SC540-family device, not as a unique raw-frame path.
- The schedule-recording sample routes MZ0380 through the same control surface used
  by SC540 product samples.
- The MZ0380 resource profile is explicitly SC540-shaped and exposes:
  - `SC540.NTSC/PAL.RESOLUTION.MAIN`
  - `SC540.NTSC/PAL.FRAMERATE`
  - `SC540.NTSC/PAL.RECORDMODE`
  - `SC540.NTSC/PAL.BITRATE`
  - `SC540.NTSC/PAL.QUALITY`
  - `SC540.VIDEO.BFRAME`
  - `SC540.VIDEO.INPUT`
- The product/sample handle split is consistent:
  - raw/live video handle: `MZ0380 PCI`
  - hardware encoder handle: `MZ0380 PCI, Analog Encoder`
  - audio handle: `MZ0380 PCI, Analog WaveIn`

## Minimum Observed Init And Control Sequence
The SC540 VB sample is the best sequence reference. The schedule-recording sample
confirms the same MZ0380 handle names and control shape, but appears to contain one
likely typo where `SET_STANDARD` is called with a type value instead of the encoder
handle.

Minimum hardware-facing sequence inferred from the samples:

1. Open or create the raw video handle for `MZ0380 PCI`.
2. Query read-only identity or capability properties:
   - serial or identity via custom property `0`
   - input capability/config via custom property `8`
3. Query or set the active input source via custom property `201`.
4. Configure the raw handle:
   - `SET_STANDARD`
   - `SET_FORMAT` to a raw preview format such as `YV12`
   - `SET_DEINTERLACE`
5. Open or create the hardware encoder handle for `MZ0380 PCI, Analog Encoder`.
6. Configure the encoder handle:
   - `SET_STANDARD`
   - `SET_FORMAT` to `H264`
   - `SET_DEINTERLACE`
   - compression and syntax properties
7. Optionally open the audio handle and set PCM format.
8. Only after raw and encoder setup, call `RUN` on the active handles.
9. Treat file renderers, network renderers, and software mux or sink objects as
   software-side graph plumbing, not part of the minimum hardware bring-up path.

This is the current best minimum init path to map onto BAR5 semantics before any
register writes are attempted.

## Sequence Nuance Across Samples
- `Form1.vb` configures the raw handle first, then the encoder, then audio, then `RUN`.
- `FileRenderer.cpp` creates raw and encoder handles together, configures the encoder
  first, then returns to raw configuration just before `RUN`.
- Common denominator:
  - raw and encoder handles both exist before streaming starts
  - input and encoder properties are set before `RUN`
  - file and network sinks are downstream consumers, not part of device bring-up

That common denominator matters more than the exact application graph order when
mapping the hardware control path.

## Input Source Mapping
`MyPeopertyDlg.cs` uses custom property `201` for input source selection:

- `0` = HDMI
- `1` = DVI-D
- `2` = COMPONENTS (YCbCr)
- `3` = DVI-A (RGB/VGA)
- `4` = SDI

This makes property `201` the clearest first control-path anchor for an eventual
write-safe source-mux experiment.

## Encoder Property Mapping
The SDK exposes encoder setup through both generic AMESDK compression-property
indices and SC540 custom-property IDs.

Practical generic-to-custom mapping inferred from the headers, PDFs, and samples:

- generic `0` -> custom `405` for GOP or key-frame rate
- generic `1` -> custom `404` for quality
- generic `3` -> custom `407` for record mode
- generic `4` -> custom `403` for bitrate
- generic `5` -> custom `408` for QP step
- generic `6` -> custom `409` for peak bitrate
- generic `7` -> custom `410` for trough or minimum bitrate or quality floor
- generic `0x0A` -> custom `411` for B-frames
- generic `0x0D` -> custom `422` for average frame rate

Other directly documented encoder-related custom properties:

- `406` force key frame
- `412` H.264 profile
- `413` aspect ratio
- `414` H.264 level
- `415` entropy mode
- `424` frame queue length

The custom-property table also groups these families in a way that is useful for
register correlation:

- `G04`: device input and receiver-facing controls such as `8`, `201`, `232`,
  `234`, `235`
- `G10`: encoder rate-control values such as `403`, `404`, `407`, `409`, `410`
- `G12`: post-average frame rate `422`
- `UE`: encoder setup values such as `405`, `406`, `408`, `411`, `412`, `413`,
  `414`, `415`

## Likely BAR5 Register Categories
These are working categories only. They are intended to constrain future read-only
and first-write experiments, not to claim register truth.

- Category A: global identity, capability, and property-router state
  - likely includes property `0`, property `8`, and other low-risk status reads
  - likely candidates for the already stable low BAR5 words near `0x0000..0x0018`
- Category B: input source and receiver-front-end control
  - properties `201`, `232`, `234`, `235`, and nearby input-signal controls
  - likely tied to HDMI or DVI or SDI mux and receiver configuration
- Category C: encoder rate-control block
  - properties `403`, `404`, `407`, `409`, `410`
  - likely mailbox-style config words rather than data-plane pointers
- Category D: encoder syntax and profile block
  - properties `405`, `406`, `408`, `411`, `412`, `413`, `414`, `415`, `422`, `424`
- Category E: OSD or overlay block
  - properties `921`, `929`
- Category F: audio-control block
  - format, gain, mute, or input-selection properties associated with the wave input

## BAR0 Versus BAR5 Working Hypothesis
- BAR5 already contains a few structured words and no obvious live-state bit from the
  current safe snapshots.
- `cfg_exp_0030 = fc200004` and `cfg_exp_0038 = fc20005f` decode into the BAR0
  physical window on this host.
- Current interpretation:
  - BAR5 is more likely the control, mailbox, or property-routing window.
  - BAR0 is more likely the data-plane, surface, queue, or DMA-related window.
- Because of that split, the next reverse-engineering step should stay focused on
  BAR5 control semantics and avoid BAR0 writes or bus-master re-enable work.

## Suggested BAR5 Correlation Order
To avoid another blind register walk, the next read-only or first-write experiments
should follow the SDK control sequence instead of scanning by address alone.

1. Read-only identity and capability correlation
   - compare BAR5 around the current low stable window while issuing or observing
     property `0` and property `8` queries
2. Input-source correlation
   - treat property `201` as the first likely write-safe control family once writes
     are allowed
   - test one change at a time such as HDMI to DVI-D and observe whether BAR5 shows
     a single-lane mailbox, selector register, or status latch update
3. Encoder rate-control correlation
   - focus next on `407`, `403` or `404`, `405`, then `411`
   - these are compact scalar values and are more likely to hit a mailbox-style BAR5
     path than data-plane memory
4. Delay `RUN` correlation until after a property-routing pattern exists
   - `RUN` is a broader state transition and is more likely to touch multiple control
     paths at once
5. Keep BAR0 out of scope during this pass
   - BAR0 correlation should resume only after BAR5 control routing is understood

## Immediate Implications For Driver Work
- Keep bus mastering disabled by default.
- Do not attempt BAR0 writes yet.
- Treat custom property `201` as the first likely source-select control anchor.
- Treat encoder properties `403` through `415`, plus `422` and `424`, as the next
  likely mailbox families to correlate against BAR5.
- Use read-only BAR5 correlation first, then minimal write experiments only after a
  candidate property-router or mailbox pattern is defensible.
