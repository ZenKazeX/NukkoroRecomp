# Active dependency snapshots

CMake uses plume (including its D3D12MemoryAllocator), stb, and the optional Tracy instrumentation client. Preserve their licenses and local source changes. Source bytes were checked against the combined candidate manifest before consolidation. These copied snapshots are not independent Git checkouts; the parent repository revision is not their revision.

Unused dependency copies, standalone profiler projects, sample/test projects, downloaded installers, and prebuilt binaries were removed. The RenderWare research SDK is in ../../RW37SDK/ and is not a runtime dependency.