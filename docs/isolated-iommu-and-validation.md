# Per-device translation and staged validation

The driver requires a translated domain, but the system-wide default may
remain `iommu=pt`. Linux exposes a per-group `type` attribute that can select
`DMA` before a driver binds. For a supported card alone in its group:

```sh
sudo bash scripts/prepare-iommu.sh <PCI-address>
```

Substitute the card's full PCI address. This helper rejects bound devices,
shared groups, enabled bus mastering, and hardware power states other than
D0. It does not load the driver, reset hardware, or persist changes across
reboots. Run without concurrent driver binding or passthrough operations;
the checks are not an atomic lock against another administrator.

The kernel may report cached power state `unknown` after driver removal.
The helper verifies the actual PCI power-management register before accepting
that state. It never tries to wake a device stuck in a low-power state.

## Allocation fallback

Large physically contiguous allocations can fail even with substantial
available memory. The video and H.264 buffer allocators now try a bounded
contiguous allocation, then use `vzalloc` pages mapped individually at the
same contiguous IOVA on x86-64. Partial mappings are unwound before freeing
backing storage. The existing coherent DMA allocation path is unchanged.

## Capture shutdown and handoff

Clearing PCI bus mastering externally does not clear the driver's cached
`dma_armed` flag. STREAMON now checks the hardware command register and
rejects a disabled or inaccessible device before starting an encoder.

For a bounded diagnostic, stop capture and unload the module before returning
the machine to ordinary camera applications. Do not leave a capture node
advertised as ready after externally disabling DMA. For an OBS handoff using
`persistent_h264=1`, keep the validated pipeline, mappings, interrupts, and
bus mastering active so OBS can attach to that same pipeline.

Disable OBS's **Use Buffering** to reduce preview delay. Keep **Deactivate
when not showing** disabled when using the persistent capture workflow.

## Validation and limitations

- Module builds with `W=1`; shell syntax and diff whitespace checks pass.
- Per-device translation was exercised with the global passthrough default.
- A bounded capture produced 90 decodable 1920x1080 H.264 frames at about
  30 delivered frames per second. Persistent handoff displayed video in OBS.
- The successful allocation run used contiguous memory: the fragmented
  fallback and partial-map failure paths have not been hardware-tested.
- The bus-master guard builds, but its rejection path needs a controlled
  hardware test. It is not a guarantee against concurrent privileged PCI
  changes during capture.
- Host PCIe failures occurred during earlier passthrough and application
  restart experiments. The precise causes remain unresolved; these changes
  must not be presented as a proven fix for those failures.
- Long-duration capture, suspend/resume, HDMI recovery, repeated lifecycle
  transitions, and audio are not validated by this contribution.

Before merging, exercise forced fragmented allocation and injected mapping
failures, verify unmap/free behavior, and test STREAMON rejection with bus
mastering disabled. A longer capture and repeated OBS attachment tests should
follow. No raw diagnostic logs or host-identifying information are included.
