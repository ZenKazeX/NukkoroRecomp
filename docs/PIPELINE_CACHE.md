# Pipeline cache (2026-11-16)

**Problem.** The renderer makes a Direct3D 12 pipeline state the first time a draw needs that combination of program,
blend factors, cull mode, depth flags, colour format and sample count (`Renderer::mesh_pipeline`, `screen_pipeline`,
`compute_pipeline`). That creation is a driver compile in the middle of a frame on the rendering thread: a hitch, once
per combination, every time the game starts. The first stage, the first explosion and the first menu each pay it.

**Fix.** The idea is ShadowRecomp's (FoxWorkshop/ShadowRecomp, `gpu/pipeline.cpp`, `gpu/video.cpp`): record every
pipeline made and make them all again at the next start. No code was taken.

- `render/pipeline_cache.h`: a pipeline as plain values (`PipelineRecord`, 16 bytes) and the file that holds them.
- **Saving.** Every new pipeline is added and the file is rewritten at once, through a temporary file renamed over it,
  so a crash keeps what was made. It is a few hundred bytes, written next to a compile that takes far longer.
- **Loading.** At `Renderer::create`, before the first frame, the cached pipelines are made (`warm_pipelines`). The
  driver's own shader cache usually makes these repeat creations fast. The log line
  `[render] pipeline cache: N of M pipelines made in X ms` gives the cost.
- **Where.** `pipeline.cache` beside `display.cfg`. `SHADOW_R2_PIPELINE_CACHE=0` turns it off for a run.
- **What is refused** (the file is ignored and rebuilt as pipelines are made):
  - a file written by a build with other shaders (the header carries a hash of every shader blob);
  - another format version;
  - a damaged file (the length and a trailing hash are checked);
  - more than 4,096 records.
- **What is skipped.** A record this GPU cannot make (an MSAA count it lacks) is skipped. That pipeline is then made at
  its first draw, as before.
- **Equivalent keys share one pipeline** (`canonical_mesh`). These are the fields the pipeline does not use:
  - a plain (blend-off) pipeline's blend factors;
  - the depth flags of a pipeline with no depth target;
  - `lines` outside the immediate program.

  This reduces the number of distinct pipelines and changes nothing on screen.

**Not done.** Driver blobs (`ID3D12PipelineLibrary` or cached PSO blobs) are not stored. They depend on the GPU and the
driver version, and the driver's shader cache already covers most of that cost. ShadowRecomp ships a pre-built cache
in the executable as well; ours starts empty on a fresh install, and the first run records it.

**Tests.** `pipeline_cache_tests` (any host) covers:
- round trip and order;
- refusal of another build's file, of every single flipped byte, of truncation and of junk;
- the key normalization;
- the cap.

The D3D12 side was compile-checked against the Windows headers, but not run here (no GPU and no disc in the session
that wrote it). On the first run, check the log line above and that a second start shows the same count made before
the first frame.
