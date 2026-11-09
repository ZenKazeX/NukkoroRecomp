// The glare sources' tile marks (docs/GLARE_OPTIMIZATION.md G2). In the glare-sources pass the draw pixel shaders are
// the GLARE_TILES variants: every pixel they write marks its 16 x 16 tile in the R32_UINT texture b0 names, so the
// resolve and the capture can leave every tile no source pixel reached (it holds the clear, black, which resolves to
// exactly 0). A separate variant because a shader that may write a UAV loses the early depth test on most hardware;
// none in SPIR-V (the Vulkan renderer has its own glare and binds no b0 here). The variants force the depth test
// before the shader (GLARE_TILES_ENTRY = [earlydepthstencil]): a UAV write otherwise moved it after, and every hidden
// source pixel ran (measured: the sources pass 0.06 -> 0.27 ms). The pass writes no depth (a read-only depth view),
// so testing early changes nothing but the hidden pixels' cost - and they no longer mark tiles.
#if defined(GLARE_TILES) && !defined(__spirv__)
#define GLARE_TILES_ENTRY [earlydepthstencil]
cbuffer GlareTiles : register(b0)
{
    uint glare_tiles;  // ResourceDescriptorHeap index of the tile marks
};
void mark_glare_tile(float4 position)
{
    RWTexture2D<uint> tiles = ResourceDescriptorHeap[glare_tiles];
    tiles[uint2(position.xy) >> 4] = 1u;
}
#else
#define GLARE_TILES_ENTRY
void mark_glare_tile(float4 position) {}
#endif
