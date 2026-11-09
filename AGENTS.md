# Machine development

This repository, Xyphemera, is a pack of Open-XY machines, each in its own
folder (`earthshine/` first) with its own definition, tests and README. Shared
tools are in `tools/`. Test a machine with `python ../open-xy/sdk.py test
--machine <folder>`.
Read README.md and use the [Open-XY SDK](https://github.com/etcetc000/open-xy)
on `staging`, normally checked out beside this repository as `../open-xy`.
Adjust that path if the user already has the SDK elsewhere.

1. Read the [machine API](https://github.com/etcetc000/open-xy/blob/staging/docs/api.md),
   follow the [UX conventions](https://github.com/etcetc000/open-xy/blob/staging/docs/ux.md)
   for controls, pages, screens and motion, and read the relevant [core machines](https://github.com/etcetc000/open-xy-core-machines):
   Macro Osc for two-page synths, Karplus or 2op FM for four-control synths,
   Ring Mod or Ping Pong for effects, Timestretch
   for sample engines. They are the worked examples; this repository holds the
   user's implementation. Use the actual parameter count: four controls keep
   the stock header. For five to eight controls, keep the generated shared
   `opxy/page_header.hpp` header and its SVG bindings: fixed rows, lowercase
   titles, active-row emphasis, any encoder press switches pages. Repeat the
   same saved parameter in columns without alternates; never invent filler
   parameters. Format captions in real units and keep artwork below y=38.
   Preserve licences and attribution when reusing code.
2. Implement through public `<opxy/...>` headers. Keep code, metadata, artwork
   and sound tests here. Do not edit the SDK to make the machine work; explain
   missing interfaces and propose a separate SDK PR against `staging`.
   Contributor code has no assembly, raw firmware addresses, OS headers or
   platform conditionals. Audio callbacks must not allocate, block, log, do I/O
   or call graphics.
3. Run `python ../open-xy/sdk.py test --machine <folder>` and extend its `tests/` for the
   machine's promised sound and behavior. Offer browser audition where supported.
   Use the host CPU estimate to guide iteration; it is not a hardware measurement.
4. After local testing and audition, propose hardware validation. Follow the SDK's
   [build guide](https://github.com/etcetc000/open-xy/blob/staging/docs/build.md)
   and [hardware profiling guide](https://github.com/etcetc000/open-xy/blob/staging/docs/profiling.md).
   Prepare a profiling-enabled build from committed source, request a device test
   if not already authorized, and wait for confirmation of installation/reboot.
   Collect a complete run (hardware profiling's "Collect a run"), generate the
   report and explain its scope.
   Never invent a CPU number from host checks. If hardware is unavailable, report
   that validation remains pending; a maintainer can run the batch.
5. If publication is requested, follow
   [catalog submission](https://github.com/etcetc000/open-xy/blob/staging/docs/catalog.md#submit-a-machine).
   New or changed draft/published entries require the hardware `cpu` record for
   the exact pinned commit. A new machine revision needs a new measurement.
   The catalog PR targets `staging` and changes only `catalog/machines.json`.

Each machine's README opens with screenshots of its screen in `<folder>/screens/`,
and its row in the pack README shows one of them (page 1 at the defaults).

Keep generated outputs outside this repository. Firmware, stock content and
firmware-containing packages must stay outside every checkout, even ignored
folders. Device operations require an explicit user instruction; building and
testing locally do not authorize installing firmware or running diagnostics.

## What the OP-XY taught us (Earthshine)

- **Keep an effect's audio stack near 1 KB.** Effects run on a small stack:
  Earthshine at 2.5 KB and 3.9 KB corrupted the device's audio (crackle on
  everything, then metallic noise). The SDK reports `audio_stack_bytes` in the
  compile report but enforces no limit, and misses frames made with `SP = SP + Pn`.
  Put scratch in the effect buffer or the state, never in big local arrays.
- **Cost is arithmetic, not memory order.** Measured on the device (`tools/probes`):
  a multiply about 3 cycles, a sequential buffer load + store about 6, the stack
  no faster (about 8), a scattered, cache-missing access about 45. Batching or
  packing delay lines did not help. Count instructions with
  `tools/blackfin-count` (about 1.7 device cycles each) and confirm with a
  profiling build; the SDK's host estimate does not track the device.
- **A profiling run's worst block includes the first, cold-cache block.**
- **Check every build links all machines** (`compile-report.json` `linked`):
  a module that overflows silently falls back to the official machines.
- **Listen before installing.** Render A/B samples (`tools/render`) of anything
  that changes the sound; the user chooses by ear.
