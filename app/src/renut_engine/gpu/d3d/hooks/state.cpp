/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <cstring>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"  // blur-nx (2026-11-10, port 76; its key 213): OcclusionQueriesOn
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/surfaces.h"  // blur-nx (2026-11-10, port 76; its key 215): NoteSurfaceHeader
#include <algorithm>

using namespace renut::gpu;

namespace {

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x82636ED0]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x82636F10]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x82636E40]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82244B18]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x3160 + i * 4)));  // katamari-nx: 0x3170->0x3160
  viewport[4] = d.W(0x3170);  // katamari-nx: 0x3180->0x3170
  viewport[5] = d.W(0x3174);  // katamari-nx: 0x3184->0x3174
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x826370E8]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x82637138]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.TouchRange(constant, 24);  // banjo-nx (2026-10-26, banjo_renut_tags)
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x821F0B40], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x8263ED90]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3474));  // katamari-nx: 0x3484->0x3474
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x821FE558]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x821F9438]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x821DE438]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x821D5060]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x821F2090]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x821F2110]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x821F2368]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x821F2460]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x82248BD0]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  NoteSurfaceHeader(surface);  // blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers)
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x82248FB0]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  NoteSurfaceHeader(surface);  // blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers)
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x82248F20]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x821F9A48]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x826411C0]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x821EFD38]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x82637208]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x826414C0]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x82634C10]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82255C98]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x82634B90]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82640010] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// blur-nx (2026-11-10/11, port 76; its key 213: blur_renut_occlusion_queries): D3DQuery_GetData's answer. Renut runs no
// GPU packets, so the counts an occlusion query waits for never come: under Renut, Issue(END) leaves the sample blocks
// as they were (zeros, then whatever the memory held) and GetData said "not ready" to every query for ever, so whatever
// a game hides by a query was never drawn (Blur's light glows, the tail lights' red glow). AnswerOcclusionQuery, called
// before the game's own GetData runs, writes into every block of an issued occlusion query what the reference's GPU
// writes at the ZPASS event (the SDK's command_processor.cpp, ExecutePacketType3_EVENT_WRITE_ZPD): the block zeroed,
// Total_A and ZPass_A = 1000 samples passed (little-endian, as the GPU writes them), and clears the issue's fence word,
// which GetData compares with the GPU's read pointer: the counts are in.
// The query object as XDK 2.0.11164's GetData reads it (blur-nx, sub_8224CD70; D3DQuery_Issue is sub_8224CAE8, the two
// functions holding the 0xFFFFFEED sentinel): type 9 (occlusion) at +4, issued (bit 0x40) in the byte at +20, the fence
// at +24, up to 30 sample-block pointers from +28 and their number at +148, 64-byte blocks (END counts at +0, BEGIN at
// +32). A game at another XDK checks its own GetData against this before it registers it.
// Not hooked in the base: Beautiful Katamari's XDK 2.0.5632 image links no D3DQuery_Issue / GetData (no instruction or
// data word of it holds 0xFFFFFEED). A game whose image has GetData adds RENUT_D3DQUERY_GETDATA_HOOK(sub_<address>) to
// the hook list below (or calls AnswerOcclusionQuery from a GetData hook of its own), and runs post-codegen again
// (tools/post_codegen.py: direct_calls.py --redo) so GetData's callers go through the hooked entry.
namespace renut::gpu::native {
namespace {
constexpr u32 kQueryFakeSamples = 1000;  // the SDK's query_occlusion_fake_sample_count
u32 QueryBe(const u8 *p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | u32(p[3]); }
void QueryLe(u8 *p, u32 v) {
  p[0] = u8(v);
  p[1] = u8(v >> 8);
  p[2] = u8(v >> 16);
  p[3] = u8(v >> 24);
}
}  // namespace

// blur-nx (2026-11-11, port 76; its key 213): a query block's physical address as the game's GetData forms it from a
// pointer at query +28: (p & 0x1FFFFFFF) + (((p >> 20) + 0x200) & 0x1000), read through the 0xC0000000 mapping. A
// pointer in the 0xE0000000 range (Blur's blocks: E6D8DFC0) is a page further on (the console's 4 KB offset of that
// range). Answers written at the pointer's own page were never read: GetData read the sentinel there and said not ready.
u32 QueryBlockPhysical(u32 p) { return (p & 0x1FFFFFFFu) + ((((p >> 20) + 0x200u) & 0x1000u)); }

bool AnswerOcclusionQuery(u32 query) {
  if (!banjo::renut_gpu::OcclusionQueriesOn())
    return false;
  u8 *q = GuestPtr<u8>(query);
  if (!q || QueryBe(q + 4) != 9 || !(q[20] & 0x40))
    return false;
  const u32 blocks = std::min<u32>(QueryBe(q + 148), 30);
  bool answered = false;
  for (u32 i = 0; i < blocks; ++i) {
    const u32 pointer = QueryBe(q + 28 + 4 * i);
    if (!pointer)
      continue;
    // Every issued query, not only blocks holding the sentinel (under Renut they hold zeros or leftover memory): the
    // reference's answer for every query is 1000 passed.
    u8 *block = GuestPhysicalPtr<u8>(QueryBlockPhysical(pointer));
    std::memset(block, 0, 64);
    QueryLe(block + 0, kQueryFakeSamples);   // Total_A
    QueryLe(block + 16, kQueryFakeSamples);  // ZPass_A
    answered = true;
  }
  if (answered) {
    std::memset(q + 24, 0, 4);
    static uint32_t logged = 0;
    if (logged++ < 4)
      RGPU_INFO("[renut] occlusion query {:08X}: {} block(s) answered with {} samples passed, as the reference's GPU "
                "does (blur_renut_occlusion_queries)", query, blocks, kQueryFakeSamples);
  }
  return answered;
}
}  // namespace renut::gpu::native

// blur-nx (2026-11-10/11, port 76; its key 213): D3DQuery_GetData(query, data, size, flags) at the game's own address:
// the answer above, then the game's GetData (its __imp__), whose result the game gets. Under the emulated renderer the
// game's GetData alone (BANJO_RENUT_HOOK). One line in the hook list: RENUT_D3DQUERY_GETDATA_HOOK(sub_8224CD70);
// (blur-nx's address; the base registers none, see AnswerOcclusionQuery).
#define RENUT_D3DQUERY_GETDATA_HOOK(subroutine)                                                                   \
  REX_IMPORT(__imp__##subroutine, renut_query_get_data_original_##subroutine, u32(u32, u32, u32, u32));          \
  static u32 renut_query_get_data_##subroutine(u32 query, u32 data, u32 size, u32 flags) {                        \
    ::renut::gpu::native::AnswerOcclusionQuery(query);                                                            \
    return renut_query_get_data_original_##subroutine(query, data, size, flags);                                  \
  }                                                                                                               \
  BANJO_RENUT_HOOK(subroutine, renut_query_get_data_##subroutine)

// Render states, by their index in N&B's table (off_82E506F0).
BANJO_RENUT_HOOK(sub_8226E9A8, SetRenderStateHook<10>);  // D3DDevice_SetRenderState_ZEnable  // katamari-nx: N&B sub_82635740, render state 10 setter (state tables, this game index 10) // nplus: sub_824DC2F8 -> sub_8226E9A8 (render state table 10)
BANJO_RENUT_HOOK(sub_8226EA18, SetRenderStateHook<11>);  // D3DDevice_SetRenderState_ZFunc  // katamari-nx: N&B sub_821CB040, render state 11 setter (state tables, this game index 11) // nplus: sub_824DC368 -> sub_8226EA18 (render state table 11)
BANJO_RENUT_HOOK(sub_8226E9E8, SetRenderStateHook<12>);  // D3DDevice_SetRenderState_ZWriteEnable  // katamari-nx: N&B sub_82635780, render state 12 setter (state tables, this game index 12) // nplus: sub_824DC338 -> sub_8226E9E8 (render state table 12)
BANJO_RENUT_HOOK(sub_8226E210, SetRenderStateHook<13>);  // D3DDevice_SetRenderState_FillMode  // katamari-nx: N&B sub_8225D148, render state 13 setter (state tables, this game index 13) // nplus: sub_824DBBD8 -> sub_8226E210 (render state table 13)
BANJO_RENUT_HOOK(sub_8226E1E0, SetRenderStateHook<14>);  // D3DDevice_SetRenderState_CullMode  // katamari-nx: N&B sub_8225D168, render state 14 setter (state tables, this game index 14) // nplus: sub_824DBBA8 -> sub_8226E1E0 (render state table 14)
BANJO_RENUT_HOOK(sub_8226E2F0, SetRenderStateHook<15>);  // D3DDevice_SetRenderState_AlphaBlendEnable  // katamari-nx: N&B sub_821F9748, render state 15 setter (state tables, this game index 15) // nplus: sub_824DBC40 -> sub_8226E2F0 (render state table 15)
BANJO_RENUT_HOOK(sub_8226E680, SetRenderStateHook<16>);  // D3DDevice_SetRenderState_SeparateAlphaBlendEnable  // katamari-nx: N&B sub_821F96C0, render state 16 setter (state tables, this game index 16) // nplus: sub_824DBFD0 -> sub_8226E680 (render state table 16)
BANJO_RENUT_HOOK(sub_8226E7B0, SetRenderStateHook<17>);  // D3DDevice_SetRenderState_BlendFactor  // katamari-nx: N&B sub_8223D118, render state 17 setter (state tables, this game index 17) // nplus: sub_824DC100 -> sub_8226E7B0 (render state table 17)
BANJO_RENUT_HOOK(sub_8226E410, SetRenderStateHook<18>);  // D3DDevice_SetRenderState_SrcBlend  // katamari-nx: N&B sub_821FF1D0, render state 18 setter (state tables, this game index 18) // nplus: sub_824DBD60 -> sub_8226E410 (render state table 18)
BANJO_RENUT_HOOK(sub_8226E4A0, SetRenderStateHook<19>);  // D3DDevice_SetRenderState_DestBlend  // katamari-nx: N&B sub_821FF8D8, render state 19 setter (state tables, this game index 19) // nplus: sub_824DBDF0 -> sub_8226E4A0 (render state table 19)
BANJO_RENUT_HOOK(sub_8226E380, SetRenderStateHook<20>);  // D3DDevice_SetRenderState_BlendOp  // katamari-nx: N&B sub_821F54F0, render state 20 setter (state tables, this game index 20) // nplus: sub_824DBCD0 -> sub_8226E380 (render state table 20)
BANJO_RENUT_HOOK(sub_8226E5A0, SetRenderStateHook<21>);  // D3DDevice_SetRenderState_SrcBlendAlpha  // katamari-nx: N&B sub_82235CD8, render state 21 setter (state tables, this game index 21) // nplus: sub_824DBEF0 -> sub_8226E5A0 (render state table 21)
BANJO_RENUT_HOOK(sub_8226E610, SetRenderStateHook<22>);  // D3DDevice_SetRenderState_DestBlendAlpha  // katamari-nx: N&B sub_821EFB00, render state 22 setter (state tables, this game index 22) // nplus: sub_824DBF60 -> sub_8226E610 (render state table 22)
BANJO_RENUT_HOOK(sub_8226E530, SetRenderStateHook<23>);  // D3DDevice_SetRenderState_BlendOpAlpha  // katamari-nx: N&B sub_821EE830, render state 23 setter (state tables, this game index 23) // nplus: sub_824DBE80 -> sub_8226E530 (render state table 23)
BANJO_RENUT_HOOK(sub_8226E240, SetRenderStateHook<24>);  // D3DDevice_SetRenderState_AlphaTestEnable  // katamari-nx: N&B sub_82635498, render state 24 setter (state tables, this game index 24) // nplus: sub_824DBC08 -> sub_8226E240 (render state table 24)
BANJO_RENUT_HOOK(sub_8226E718, SetRenderStateHook<25>);  // D3DDevice_SetRenderState_AlphaRef  // katamari-nx: N&B sub_82635550, render state 25 setter (state tables, this game index 25) // nplus: sub_824DC068 -> sub_8226E718 (render state table 25)
BANJO_RENUT_HOOK(sub_8226E780, SetRenderStateHook<26>);  // D3DDevice_SetRenderState_AlphaFunc  // katamari-nx: N&B sub_826355B8, render state 26 setter (state tables, this game index 26) // nplus: sub_824DC0D0 -> sub_8226E780 (render state table 26)
BANJO_RENUT_HOOK(sub_8226EA50, SetRenderStateHook<27>);  // D3DDevice_SetRenderState_StencilEnable  // katamari-nx: N&B sub_826357C0, render state 27 setter (state tables, this game index 27) // nplus: sub_824DC3A0 -> sub_8226EA50 (render state table 27)
BANJO_RENUT_HOOK(sub_8226EA90, SetRenderStateHook<28>);  // D3DDevice_SetRenderState_TwoSidedStencilMode  // katamari-nx: N&B sub_82635800, render state 28 setter (state tables, this game index 28) // nplus: sub_824DC3E0 -> sub_8226EA90 (render state table 28)
BANJO_RENUT_HOOK(sub_8226EAF8, SetRenderStateHook<29>);  // D3DDevice_SetRenderState_StencilFail  // katamari-nx: N&B sub_82635868, render state 29 setter (state tables, this game index 29) // nplus: sub_824DC448 -> sub_8226EAF8 (render state table 29)
BANJO_RENUT_HOOK(sub_8226EB30, SetRenderStateHook<30>);  // D3DDevice_SetRenderState_StencilZFail  // katamari-nx: N&B sub_826358A0, render state 30 setter (state tables, this game index 30) // nplus: sub_824DC480 -> sub_8226EB30 (render state table 30)
BANJO_RENUT_HOOK(sub_8226EB68, SetRenderStateHook<31>);  // D3DDevice_SetRenderState_StencilPass  // katamari-nx: N&B sub_826358D8, render state 31 setter (state tables, this game index 31) // nplus: sub_824DC4B8 -> sub_8226EB68 (render state table 31)
BANJO_RENUT_HOOK(sub_8226EAC8, SetRenderStateHook<32>);  // D3DDevice_SetRenderState_StencilFunc  // katamari-nx: N&B sub_82635838, render state 32 setter (state tables, this game index 32) // nplus: sub_824DC418 -> sub_8226EAC8 (render state table 32)
BANJO_RENUT_HOOK(sub_8226EC68, SetRenderStateHook<33>);  // D3DDevice_SetRenderState_StencilRef  // katamari-nx: N&B sub_826359D8, render state 33 setter (state tables, this game index 33) // nplus: sub_824DC5B8 -> sub_8226EC68 (render state table 33)
BANJO_RENUT_HOOK(sub_8226EC88, SetRenderStateHook<34>);  // D3DDevice_SetRenderState_StencilMask  // katamari-nx: N&B sub_826359F8, render state 34 setter (state tables, this game index 34) // nplus: sub_824DC5D8 -> sub_8226EC88 (render state table 34)
BANJO_RENUT_HOOK(sub_8226ECA8, SetRenderStateHook<35>);  // D3DDevice_SetRenderState_StencilWriteMask  // katamari-nx: N&B sub_82635A18, render state 35 setter (state tables, this game index 35) // nplus: sub_824DC5F8 -> sub_8226ECA8 (render state table 35)
BANJO_RENUT_HOOK(sub_8226EBC8, SetRenderStateHook<36>);  // D3DDevice_SetRenderState_CCWStencilFail  // katamari-nx: N&B sub_82635938, render state 36 setter (state tables, this game index 36) // nplus: sub_824DC518 -> sub_8226EBC8 (render state table 36)
BANJO_RENUT_HOOK(sub_8226EC00, SetRenderStateHook<37>);  // D3DDevice_SetRenderState_CCWStencilZFail  // katamari-nx: N&B sub_82635970, render state 37 setter (state tables, this game index 37) // nplus: sub_824DC550 -> sub_8226EC00 (render state table 37)
BANJO_RENUT_HOOK(sub_8226EC38, SetRenderStateHook<38>);  // D3DDevice_SetRenderState_CCWStencilPass  // katamari-nx: N&B sub_826359A8, render state 38 setter (state tables, this game index 38) // nplus: sub_824DC588 -> sub_8226EC38 (render state table 38)
BANJO_RENUT_HOOK(sub_8226EB98, SetRenderStateHook<39>);  // D3DDevice_SetRenderState_CCWStencilFunc  // katamari-nx: N&B sub_82635908, render state 39 setter (state tables, this game index 39) // nplus: sub_824DC4E8 -> sub_8226EB98 (render state table 39)
BANJO_RENUT_HOOK(sub_8226ECC8, SetRenderStateHook<40>);  // D3DDevice_SetRenderState_CCWStencilRef  // katamari-nx: N&B sub_82635A38, render state 40 setter (state tables, this game index 40) // nplus: sub_824DC618 -> sub_8226ECC8 (render state table 40)
BANJO_RENUT_HOOK(sub_8226ECE8, SetRenderStateHook<41>);  // D3DDevice_SetRenderState_CCWStencilMask  // katamari-nx: N&B sub_82635A58, render state 41 setter (state tables, this game index 41) // nplus: sub_824DC638 -> sub_8226ECE8 (render state table 41)
BANJO_RENUT_HOOK(sub_8226ED08, SetRenderStateHook<42>);  // D3DDevice_SetRenderState_CCWStencilWriteMask  // katamari-nx: N&B sub_82635A78, render state 42 setter (state tables, this game index 42) // nplus: sub_824DC658 -> sub_8226ED08 (render state table 42)
BANJO_RENUT_HOOK(sub_8226ED28, SetRenderStateHook<43>);  // D3DDevice_SetRenderState_ClipPlaneEnable  // katamari-nx: N&B sub_8225D188, render state 43 setter (state tables, this game index 43) // nplus: sub_824DC678 -> sub_8226ED28 (render state table 43)
BANJO_RENUT_HOOK(sub_8226F068, SetRenderStateHook<44>);  // D3DDevice_SetRenderState_PointSize  // katamari-nx: N&B sub_82635CC0, render state 44 setter (state tables, this game index 44) // nplus: sub_824DC9A0 -> sub_8226F068 (render state table 44)
BANJO_RENUT_HOOK(sub_8226F0C8, SetRenderStateHook<45>);  // D3DDevice_SetRenderState_PointSizeMin  // katamari-nx: N&B sub_82635D20, render state 45 setter (state tables, this game index 45) // nplus: sub_824DCA00 -> sub_8226F0C8 (render state table 45)
BANJO_RENUT_HOOK(sub_8226F058, SetRenderStateHook<46>);  // D3DDevice_SetRenderState_PointSpriteEnable  // katamari-nx: N&B sub_82635CB0, render state 46 setter (state tables, this game index 46) // nplus: sub_824DC990 -> sub_8226F058 (render state table 46)
BANJO_RENUT_HOOK(sub_8226F120, SetRenderStateHook<47>);  // D3DDevice_SetRenderState_PointSizeMax  // katamari-nx: N&B sub_822371C0, render state 47 setter (state tables, this game index 47) // nplus: sub_824DCA58 -> sub_8226F120 (render state table 47)
BANJO_RENUT_HOOK(sub_8226EF08, SetRenderStateHook<48>);  // D3DDevice_SetRenderState_MultiSampleAntiAlias  // katamari-nx: N&B sub_82635B98, render state 48 setter (state tables, this game index 48) // nplus: sub_824DC840 -> sub_8226EF08 (render state table 48)
BANJO_RENUT_HOOK(sub_8226EF38, SetRenderStateHook<49>);  // D3DDevice_SetRenderState_MultiSampleMask  // katamari-nx: N&B sub_82635BC8, render state 49 setter (state tables, this game index 49) // nplus: sub_824DC870 -> sub_8226EF38 (render state table 49)
BANJO_RENUT_HOOK(sub_8226ED78, SetRenderStateHook<50>);  // D3DDevice_SetRenderState_ScissorTestEnable  // katamari-nx: N&B sub_82635AA8, render state 50 setter (state tables, this game index 50) // nplus: sub_824DE720 -> sub_8226ED78 (render state table 50)
BANJO_RENUT_HOOK(sub_8226ED90, SetRenderStateHook<51>);  // D3DDevice_SetRenderState_SlopeScaleDepthBias  // katamari-nx: N&B sub_82635AC0, render state 51 setter (state tables, this game index 51) // nplus: sub_824DC6D0 -> sub_8226ED90 (render state table 51)
BANJO_RENUT_HOOK(sub_8226EE58, SetRenderStateHook<52>);  // D3DDevice_SetRenderState_DepthBias  // katamari-nx: N&B sub_821CAFA8, render state 52 setter (state tables, this game index 52) // nplus: sub_824DC798 -> sub_8226EE58 (render state table 52)
BANJO_RENUT_HOOK(sub_8226EF58, SetRenderStateHook<53>);  // D3DDevice_SetRenderState_ColorWriteEnable  // katamari-nx: N&B sub_821CAF70, render state 53 setter (state tables, this game index 53) // nplus: sub_824DC890 -> sub_8226EF58 (render state table 53)
BANJO_RENUT_HOOK(sub_8226EF98, SetRenderStateHook<54>);  // D3DDevice_SetRenderState_ColorWriteEnable1  // katamari-nx: N&B sub_82635BF0, render state 54 setter (state tables, this game index 54) // nplus: sub_824DC8D0 -> sub_8226EF98 (render state table 54)
BANJO_RENUT_HOOK(sub_8226EFD8, SetRenderStateHook<55>);  // D3DDevice_SetRenderState_ColorWriteEnable2  // katamari-nx: N&B sub_82635C30, render state 55 setter (state tables, this game index 55) // nplus: sub_824DC910 -> sub_8226EFD8 (render state table 55)
BANJO_RENUT_HOOK(sub_8226F018, SetRenderStateHook<56>);  // D3DDevice_SetRenderState_ColorWriteEnable3  // katamari-nx: N&B sub_82635C70, render state 56 setter (state tables, this game index 56) // nplus: sub_824DC950 -> sub_8226F018 (render state table 56)
BANJO_RENUT_HOOK(sub_8226FA10, SetRenderStateHook<57>);  // D3DDevice_SetRenderState_TessellationMode  // katamari-nx: N&B sub_826363C8, render state 57 setter (state tables, this game index 57) // nplus: sub_824DD190 -> sub_8226FA10 (render state table 57)
BANJO_RENUT_HOOK(sub_8226F9A0, SetRenderStateHook<58>);  // D3DDevice_SetRenderState_MinTessellationLevel  // katamari-nx: N&B sub_82636358, render state 58 setter (state tables, this game index 58) // nplus: sub_824DD120 -> sub_8226F9A0 (render state table 58)
BANJO_RENUT_HOOK(sub_8226F9D8, SetRenderStateHook<59>);  // D3DDevice_SetRenderState_MaxTessellationLevel  // katamari-nx: N&B sub_82636390, render state 59 setter (state tables, this game index 59) // nplus: sub_824DD158 -> sub_8226F9D8 (render state table 59)
BANJO_RENUT_HOOK(sub_8226F1F8, SetRenderStateHook<60>);  // D3DDevice_SetRenderState_Wrap0  // katamari-nx: N&B sub_82635D88, render state 60 setter (state tables, this game index 60) // nplus: sub_824DCAB0 -> sub_8226F1F8 (render state table 60)
BANJO_RENUT_HOOK(sub_8226F218, SetRenderStateHook<61>);  // D3DDevice_SetRenderState_Wrap1  // katamari-nx: N&B sub_82635DA8, render state 61 setter (state tables, this game index 61) // nplus: sub_824DCAD0 -> sub_8226F218 (render state table 61)
BANJO_RENUT_HOOK(sub_8226F240, SetRenderStateHook<62>);  // D3DDevice_SetRenderState_Wrap2  // katamari-nx: N&B sub_82635DD0, render state 62 setter (state tables, this game index 62) // nplus: sub_824DCAF8 -> sub_8226F240 (render state table 62)
BANJO_RENUT_HOOK(sub_8226F268, SetRenderStateHook<63>);  // D3DDevice_SetRenderState_Wrap3  // katamari-nx: N&B sub_82635DF8, render state 63 setter (state tables, this game index 63) // nplus: sub_824DCB20 -> sub_8226F268 (render state table 63)
BANJO_RENUT_HOOK(sub_8226F290, SetRenderStateHook<64>);  // D3DDevice_SetRenderState_Wrap4  // katamari-nx: N&B sub_82635E20, render state 64 setter (state tables, this game index 64) // nplus: sub_824DCB48 -> sub_8226F290 (render state table 64)
BANJO_RENUT_HOOK(sub_8226F2B8, SetRenderStateHook<65>);  // D3DDevice_SetRenderState_Wrap5  // katamari-nx: N&B sub_82635E48, render state 65 setter (state tables, this game index 65) // nplus: sub_824DCB70 -> sub_8226F2B8 (render state table 65)
BANJO_RENUT_HOOK(sub_8226F2E0, SetRenderStateHook<66>);  // D3DDevice_SetRenderState_Wrap6  // katamari-nx: N&B sub_82635E70, render state 66 setter (state tables, this game index 66) // nplus: sub_824DCB98 -> sub_8226F2E0 (render state table 66)
BANJO_RENUT_HOOK(sub_8226F308, SetRenderStateHook<67>);  // D3DDevice_SetRenderState_Wrap7  // katamari-nx: N&B sub_82635E98, render state 67 setter (state tables, this game index 67) // nplus: sub_824DCBC0 -> sub_8226F308 (render state table 67)
BANJO_RENUT_HOOK(sub_8226F328, SetRenderStateHook<68>);  // D3DDevice_SetRenderState_Wrap8  // katamari-nx: N&B sub_82635EB8, render state 68 setter (state tables, this game index 68) // nplus: sub_824DCBE0 -> sub_8226F328 (render state table 68)
BANJO_RENUT_HOOK(sub_8226F348, SetRenderStateHook<69>);  // D3DDevice_SetRenderState_Wrap9  // katamari-nx: N&B sub_82635ED8, render state 69 setter (state tables, this game index 69) // nplus: sub_824DCC00 -> sub_8226F348 (render state table 69)
BANJO_RENUT_HOOK(sub_8226F370, SetRenderStateHook<70>);  // D3DDevice_SetRenderState_Wrap10  // katamari-nx: N&B sub_82635F00, render state 70 setter (state tables, this game index 70) // nplus: sub_824DCC28 -> sub_8226F370 (render state table 70)
BANJO_RENUT_HOOK(sub_8226F398, SetRenderStateHook<71>);  // D3DDevice_SetRenderState_Wrap11  // katamari-nx: N&B sub_82635F28, render state 71 setter (state tables, this game index 71) // nplus: sub_824DCC50 -> sub_8226F398 (render state table 71)
BANJO_RENUT_HOOK(sub_8226F3C0, SetRenderStateHook<72>);  // D3DDevice_SetRenderState_Wrap12  // katamari-nx: N&B sub_82635F50, render state 72 setter (state tables, this game index 72) // nplus: sub_824DCC78 -> sub_8226F3C0 (render state table 72)
BANJO_RENUT_HOOK(sub_8226F3E8, SetRenderStateHook<73>);  // D3DDevice_SetRenderState_Wrap13  // katamari-nx: N&B sub_82635F78, render state 73 setter (state tables, this game index 73) // nplus: sub_824DCCA0 -> sub_8226F3E8 (render state table 73)
BANJO_RENUT_HOOK(sub_8226F410, SetRenderStateHook<74>);  // D3DDevice_SetRenderState_Wrap14  // katamari-nx: N&B sub_82635FA0, render state 74 setter (state tables, this game index 74) // nplus: sub_824DCCC8 -> sub_8226F410 (render state table 74)
BANJO_RENUT_HOOK(sub_8226F438, SetRenderStateHook<75>);  // D3DDevice_SetRenderState_Wrap15  // katamari-nx: N&B sub_82635FC8, render state 75 setter (state tables, this game index 75) // nplus: sub_824DCCF0 -> sub_8226F438 (render state table 75)
BANJO_RENUT_HOOK(sub_8226F558, SetRenderStateHook<76>);  // D3DDevice_SetRenderState_ViewportEnable  // katamari-nx: N&B sub_821D53E8, render state 76 setter (state tables, this game index 76) // nplus: sub_824DCE10 -> sub_8226F558 (render state table 76)
BANJO_RENUT_HOOK(sub_8226F6E0, SetRenderStateHook<77>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable  // katamari-nx: N&B sub_826360F8, render state 77 setter (state tables, this game index 77) // nplus: sub_824DCE60 -> sub_8226F6E0 (render state table 77)
BANJO_RENUT_HOOK(sub_8226F790, SetRenderStateHook<78>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable1  // katamari-nx: N&B sub_82636190, render state 78 setter (state tables, this game index 78) // nplus: sub_824DCF10 -> sub_8226F790 (render state table 78)
BANJO_RENUT_HOOK(sub_8226F840, SetRenderStateHook<79>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable2  // katamari-nx: N&B sub_82636228, render state 79 setter (state tables, this game index 79) // nplus: sub_824DCFC0 -> sub_8226F840 (render state table 79)
BANJO_RENUT_HOOK(sub_8226F8F0, SetRenderStateHook<80>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable3  // katamari-nx: N&B sub_826362C0, render state 80 setter (state tables, this game index 80) // nplus: sub_824DD070 -> sub_8226F8F0 (render state table 80)
BANJO_RENUT_HOOK(sub_8226FA48, SetRenderStateHook<81>);  // D3DDevice_SetRenderState_HalfPixelOffset  // katamari-nx: N&B sub_82636400, render state 81 setter (state tables, this game index 81) // nplus: sub_824DD1C8 -> sub_8226FA48 (render state table 81)
BANJO_RENUT_HOOK(sub_8226FA80, SetRenderStateHook<82>);  // D3DDevice_SetRenderState_PrimitiveResetEnable  // katamari-nx: N&B sub_82636438, render state 82 setter (state tables, this game index 82) // nplus: sub_824DD200 -> sub_8226FA80 (render state table 82)
BANJO_RENUT_HOOK(sub_8226FAB0, SetRenderStateHook<83>);  // D3DDevice_SetRenderState_PrimitiveResetIndex  // katamari-nx: N&B sub_82636468, render state 83 setter (state tables, this game index 83) // nplus: sub_824DD230 -> sub_8226FAB0 (render state table 83)
BANJO_RENUT_HOOK(sub_8226FAD8, SetRenderStateHook<84>);  // D3DDevice_SetRenderState_AlphaToMaskEnable  // katamari-nx: N&B sub_82636490, render state 84 setter (state tables, this game index 84) // nplus: sub_824DD258 -> sub_8226FAD8 (render state table 84)
BANJO_RENUT_HOOK(sub_8226FB08, SetRenderStateHook<85>);  // D3DDevice_SetRenderState_AlphaToMaskOffsets  // katamari-nx: N&B sub_826364C0, render state 85 setter (state tables, this game index 85) // nplus: sub_824DD288 -> sub_8226FB08 (render state table 85)
BANJO_RENUT_HOOK(sub_8226FB40, SetRenderStateHook<86>);  // D3DDevice_SetRenderState_GuardBand_X  // katamari-nx: N&B sub_826364E8, render state 86 setter (state tables, this game index 86) // nplus: sub_824DD2C0 -> sub_8226FB40 (render state table 86)
BANJO_RENUT_HOOK(sub_8226FB70, SetRenderStateHook<87>);  // D3DDevice_SetRenderState_GuardBand_Y  // katamari-nx: N&B sub_82636518, render state 87 setter (state tables, this game index 87) // nplus: sub_824DD2F0 -> sub_8226FB70 (render state table 87)
BANJO_RENUT_HOOK(sub_8226FBA0, SetRenderStateHook<88>);  // D3DDevice_SetRenderState_DiscardBand_X  // katamari-nx: N&B sub_82636548, render state 88 setter (state tables, this game index 88) // nplus: sub_824DD320 -> sub_8226FBA0 (render state table 88)
BANJO_RENUT_HOOK(sub_8226FBC8, SetRenderStateHook<89>);  // D3DDevice_SetRenderState_DiscardBand_Y  // katamari-nx: N&B sub_82636570, render state 89 setter (state tables, this game index 89) // nplus: sub_824DD348 -> sub_8226FBC8 (render state table 89)
BANJO_RENUT_HOOK(sub_8226FC90, SetRenderStateHook<90>);  // D3DDevice_SetRenderState_HiStencilEnable  // katamari-nx: N&B sub_826365E0, render state 90 setter (state tables, this game index 90) // nplus: sub_824DD410 -> sub_8226FC90 (render state table 90)
BANJO_RENUT_HOOK(sub_8226FCC0, SetRenderStateHook<91>);  // D3DDevice_SetRenderState_HiStencilWriteEnable  // katamari-nx: N&B sub_82636610, render state 91 setter (state tables, this game index 91) // nplus: sub_824DD440 -> sub_8226FCC0 (render state table 91)
BANJO_RENUT_HOOK(sub_8226FCF0, SetRenderStateHook<92>);  // D3DDevice_SetRenderState_HiStencilFunc  // katamari-nx: N&B sub_82636640, render state 92 setter (state tables, this game index 92) // nplus: sub_824DD470 -> sub_8226FCF0 (render state table 92)
BANJO_RENUT_HOOK(sub_8226FD20, SetRenderStateHook<93>);  // D3DDevice_SetRenderState_HiStencilRef  // katamari-nx: N&B sub_82636670, render state 93 setter (state tables, this game index 93) // nplus: sub_824DD4A0 -> sub_8226FD20 (render state table 93)
BANJO_RENUT_HOOK(sub_8226FD40, SetRenderStateHook<94>);  // D3DDevice_SetRenderState_PresentInterval  // katamari-nx: N&B sub_82636690, render state 94 setter (state tables, this game index 94) // nplus: sub_824DD4C0 -> sub_8226FD40 (render state table 94)
BANJO_RENUT_HOOK(sub_8226FD50, SetRenderStateHook<95>);  // D3DDevice_SetRenderState_PresentImmediateThreshold  // katamari-nx: N&B sub_826366A0, render state 95 setter (state tables, this game index 95) // nplus: sub_824DD4D0 -> sub_8226FD50 (render state table 95)
BANJO_RENUT_HOOK(sub_8226FBF8, SetRenderStateHook<96>);  // D3DDevice_SetRenderState_HiZEnable  // katamari-nx: N&B sub_826365A0, render state 96 setter (state tables, this game index 96) // nplus: sub_824DD378 -> sub_8226FBF8 (render state table 96)
BANJO_RENUT_HOOK(sub_8226FC28, SetRenderStateHook<97>);  // D3DDevice_SetRenderState_HiZWriteEnable  // katamari-nx: N&B sub_82257938, render state 97 setter (state tables, this game index 97) // nplus: sub_824DD3A8 -> sub_8226FC28 (render state table 97)
BANJO_RENUT_HOOK(sub_8226E8D0, SetRenderStateHook<98>);  // D3DDevice_SetRenderState_LastPixel  // katamari-nx: N&B sub_82635660, render state 98 setter (state tables, this game index 98) // nplus: sub_824DC220 -> sub_8226E8D0 (render state table 98)
BANJO_RENUT_HOOK(sub_8226E908, SetRenderStateHook<99>);  // D3DDevice_SetRenderState_LineWidth  // katamari-nx: N&B sub_82635698, render state 99 setter (state tables, this game index 99) // nplus: sub_824DC258 -> sub_8226E908 (render state table 99)
BANJO_RENUT_HOOK(sub_8226E978, SetRenderStateHook<100>);  // D3DDevice_SetRenderState_Buffer2Frames  // katamari-nx: N&B sub_82635710, render state 100 setter (state tables, this game index 100) // nplus: sub_824DC2C8 -> sub_8226E978 (render state table 100)

// Sampler states, by their index in N&B's table (off_82E50BD0).
BANJO_RENUT_HOOK(sub_822704B0, SetSamplerStateHook<0>);  // D3DDevice_SetSamplerState_AddressU  // katamari-nx: N&B sub_8224B2C0, sampler state 0 setter (state tables, this game index 0) // nplus: sub_824DDC30 -> sub_822704B0 (sampler state table 0)
BANJO_RENUT_HOOK(sub_82270500, SetSamplerStateHook<1>);  // D3DDevice_SetSamplerState_AddressV  // katamari-nx: N&B sub_821DB8C8, sampler state 1 setter (state tables, this game index 1) // nplus: sub_824DDC80 -> sub_82270500 (sampler state table 1)
BANJO_RENUT_HOOK(sub_82270550, SetSamplerStateHook<2>);  // D3DDevice_SetSamplerState_AddressW  // katamari-nx: N&B sub_82636C30, sampler state 2 setter (state tables, this game index 2) // nplus: sub_824DDCD0 -> sub_82270550 (sampler state table 2)
BANJO_RENUT_HOOK(sub_82270440, SetSamplerStateHook<3>);  // D3DDevice_SetSamplerState_BorderColor  // katamari-nx: N&B sub_82636B90, sampler state 3 setter (state tables, this game index 3) // nplus: sub_824DDBC0 -> sub_82270440 (sampler state table 3)
BANJO_RENUT_HOOK(sub_8226FF00, SetSamplerStateHook<4>);  // D3DDevice_SetSamplerState_MagFilter  // katamari-nx: N&B sub_821FE308, sampler state 4 setter (state tables, this game index 4) // nplus: sub_824DD680 -> sub_8226FF00 (sampler state table 4)
BANJO_RENUT_HOOK(sub_8226FD70, SetSamplerStateHook<5>);  // D3DDevice_SetSamplerState_MinFilter  // katamari-nx: N&B sub_821FE3D0, sampler state 5 setter (state tables, this game index 5) // nplus: sub_824DD4F0 -> sub_8226FD70 (sampler state table 5)
BANJO_RENUT_HOOK(sub_82270090, SetSamplerStateHook<6>);  // D3DDevice_SetSamplerState_MipFilter  // katamari-nx: N&B sub_8225D108, sampler state 6 setter (state tables, this game index 6) // nplus: sub_824DD810 -> sub_82270090 (sampler state table 6)
BANJO_RENUT_HOOK(sub_822702A0, SetSamplerStateHook<7>);  // D3DDevice_SetSamplerState_MipMapLodBias  // katamari-nx: N&B sub_82636A58, sampler state 7 setter (state tables, this game index 7) // nplus: sub_824DDA20 -> sub_822702A0 (sampler state table 7)
BANJO_RENUT_HOOK(sub_82270340, SetSamplerStateHook<8>);  // D3DDevice_SetSamplerState_MaxMipLevel  // katamari-nx: N&B sub_821FE6D8, sampler state 8 setter (state tables, this game index 8) // nplus: sub_824DDAC0 -> sub_82270340 (sampler state table 8)
BANJO_RENUT_HOOK(sub_82270180, SetSamplerStateHook<9>);  // D3DDevice_SetSamplerState_MaxAnisotropy  // katamari-nx: N&B sub_82636938, sampler state 9 setter (state tables, this game index 9) // nplus: sub_824DD900 -> sub_82270180 (sampler state table 9)
BANJO_RENUT_HOOK(sub_8226FFF8, SetSamplerStateHook<10>);  // D3DDevice_SetSamplerState_MagFilterZ  // katamari-nx: N&B sub_826367D8, sampler state 10 setter (state tables, this game index 10) // nplus: sub_824DD778 -> sub_8226FFF8 (sampler state table 10)
BANJO_RENUT_HOOK(sub_8226FE68, SetSamplerStateHook<11>);  // D3DDevice_SetSamplerState_MinFilterZ  // katamari-nx: N&B sub_826366F8, sampler state 11 setter (state tables, this game index 11) // nplus: sub_824DD5E8 -> sub_8226FE68 (sampler state table 11)
BANJO_RENUT_HOOK(sub_822700E8, SetSamplerStateHook<12>);  // D3DDevice_SetSamplerState_SeparateZFilterEnable  // katamari-nx: N&B sub_82636890, sampler state 12 setter (state tables, this game index 12) // nplus: sub_824DD868 -> sub_822700E8 (sampler state table 12)
BANJO_RENUT_HOOK(sub_822703C0, SetSamplerStateHook<13>);  // D3DDevice_SetSamplerState_MinMipLevel  // katamari-nx: N&B sub_82636B10, sampler state 13 setter (state tables, this game index 13) // nplus: sub_824DDB40 -> sub_822703C0 (sampler state table 13)
BANJO_RENUT_HOOK(sub_822705A0, SetSamplerStateHook<14>);  // D3DDevice_SetSamplerState_TrilinearThreshold  // katamari-nx: N&B sub_82636C80, sampler state 14 setter (state tables, this game index 14) // nplus: sub_824DDD20 -> sub_822705A0 (sampler state table 14)
BANJO_RENUT_HOOK(sub_822701F8, SetSamplerStateHook<15>);  // D3DDevice_SetSamplerState_AnisotropyBias  // katamari-nx: N&B sub_826369B0, sampler state 15 setter (state tables, this game index 15) // nplus: sub_824DD978 -> sub_822701F8 (sampler state table 15)
BANJO_RENUT_HOOK(sub_822705F8, SetSamplerStateHook<16>);  // D3DDevice_SetSamplerState_HGradientExpBias  // katamari-nx: N&B sub_82636CD8, sampler state 16 setter (state tables, this game index 16) // nplus: sub_824DDD78 -> sub_822705F8 (sampler state table 16)
BANJO_RENUT_HOOK(sub_82270650, SetSamplerStateHook<17>);  // D3DDevice_SetSamplerState_VGradientExpBias  // katamari-nx: N&B sub_82636D30, sampler state 17 setter (state tables, this game index 17) // nplus: sub_824DDDD0 -> sub_82270650 (sampler state table 17)
BANJO_RENUT_HOOK(sub_822706A8, SetSamplerStateHook<18>);  // D3DDevice_SetSamplerState_WhiteBorderColorW  // katamari-nx: N&B sub_82636D88, sampler state 18 setter (state tables, this game index 18) // nplus: sub_824DDE28 -> sub_822706A8 (sampler state table 18)
BANJO_RENUT_HOOK(sub_82270700, SetSamplerStateHook<19>);  // D3DDevice_SetSamplerState_PointBorderEnable  // katamari-nx: N&B sub_82636DE0, sampler state 19 setter (state tables, this game index 19) // nplus: sub_824DDE80 -> sub_82270700 (sampler state table 19)

BANJO_RENUT_HOOK(sub_8227DDB0, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture  // katamari-nx: N&B sub_821FE558, signature 0.82 (next 0.39) // nplus: sub_824D2640 -> sub_8227DDB0 (hand)
BANJO_RENUT_UNHOOKED_HOOK(sub_824A3D40, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture_Inline  // katamari-nx: N&B sub_82BF2578, signature 1.00 (next 0.71) // nplus: unhooked, SetTexture_Inline: compiled into game code in N+ (the slot-index writers 8227DF30, 822703C0, 8227034
BANJO_RENUT_HOOK(sub_82270940, D3DDevice_SetStreamSource_hook);  // D3DDevice_SetStreamSource  // katamari-nx: N&B sub_821F9438, signature 0.83 (next 0.38) // nplus: sub_824DDFE0 -> sub_82270940 (hand)
BANJO_RENUT_HOOK(sub_82270AE8, D3DDevice_SetIndices_hook);  // D3DDevice_SetIndices  // katamari-nx: N&B sub_821DE438, signature 0.76 (next 0.34) // nplus: sub_824DE188 -> sub_82270AE8 (hand)
BANJO_RENUT_HOOK(sub_82261A70, D3DDevice_SetVertexDeclaration_hook);  // D3DDevice_SetVertexDeclaration  // katamari-nx: N&B sub_821D5060, D3DDevice_SetVertexDeclaration: 5 instructions, VertexDeclaration +11812 (N&B +11824); sig 0.67/0.43 // nplus: sub_824D4B48 -> sub_82261A70 (hand)
BANJO_RENUT_UNHOOKED_HOOK(sub_824DE730, D3DDevice_SetViewport_hook);  // D3DDevice_SetViewport  // katamari-nx: N&B sub_821F2090, D3DDevice_SetViewport: sig 0.63/0.30 // nplus: unhooked, inline in N+'s game code (viewport +12640 written by sub_8226DF68 inside Present/BeginTiling); rende
BANJO_RENUT_UNHOOKED_HOOK(sub_824DE4B8, D3D_SetViewport_hook);  // D3D_SetViewport  // katamari-nx: N&B sub_821F2110, D3D_SetViewport: sig 0.64/0.27 // nplus: unhooked, as SetViewport
BANJO_RENUT_HOOK(sub_82270760, D3DDevice_SetScissorRect_hook);  // D3DDevice_SetScissorRect  // katamari-nx: N&B sub_821F2368, D3DDevice_SetScissorRect: Present's tail calls it; sig 0.49/0.24 // nplus: sub_824DDEE0 -> sub_82270760 (hand)
BANJO_RENUT_UNHOOKED_HOOK(sub_824DB8C8, D3D_SetSurfaceClip_hook);  // D3D_SetSurfaceClip  // katamari-nx: N&B sub_821F2460, signature 0.83 (next 0.37) // nplus: unhooked, not found yet
BANJO_RENUT_HOOK(sub_82270BB8, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget  // katamari-nx: N&B sub_82248BD0, D3DDevice_SetRenderTarget: InitializePresentationParameters calls it after CreateSurface, as N&B's; sig 0.62/0.31 // nplus: sub_824DE868 -> sub_82270BB8 (hand)
BANJO_RENUT_HOOK(sub_82270F20, D3DDevice_SetDepthStencilSurface_hook);  // D3DDevice_SetDepthStencilSurface  // katamari-nx: N&B sub_82248FB0, D3DDevice_SetDepthStencilSurface: likewise after the depth surface; sig 0.71/0.30 // nplus: sub_824DEBD0 -> sub_82270F20 (hand)
BANJO_RENUT_UNHOOKED_HOOK(sub_824DE7D0, D3D_SetSurfaceInfo_hook);  // D3D_SetSurfaceInfo  // katamari-nx: N&B sub_82248F20, D3D_SetSurfaceInfo: sig 0.57/0.19; full viewport 0x820FAEB4 // nplus: unhooked, not found yet
BANJO_RENUT_UNHOOKED_HOOK(sub_824EFA58, D3DDevice_SetPredication_hook);  // D3DDevice_SetPredication  // katamari-nx: N&B sub_821F9A48, D3DDevice_SetPredication: sig 0.69 (SetCommandBufferPredication's best is the same function at 0.63: XDK 2.0.5632 has one) // nplus: unhooked, several writers of +12700; not settled
BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_826411C0, D3DDevice_SetCommandBufferPredication_hook);  // D3DDevice_SetCommandBufferPredication  // katamari-nx: not hooked (D3DDevice_SetCommandBufferPredication: see SetPredication)
BANJO_RENUT_UNHOOKED_HOOK(sub_824D4CD8, D3DDevice_SetShaderGPRAllocation_hook);  // D3DDevice_SetShaderGPRAllocation  // katamari-nx: N&B sub_821EFD38, signature 0.78 (next 0.30) // nplus: unhooked, not found yet
BANJO_RENUT_UNHOOKED_HOOK(sub_824D3FE0, D3D_CleanupGprAllocationSetup_hook);  // D3D_CleanupGprAllocationSetup  // katamari-nx: N&B sub_82637208, signature 0.98 (next 0.13) // nplus: unhooked, not found yet
BANJO_RENUT_HOOK(sub_82280D90, D3DDevice_SetScreenExtentQueryMode_hook);  // D3DDevice_SetScreenExtentQueryMode  // katamari-nx: N&B sub_826414C0, signature 1.00 (next 0.75) // nplus: sub_824E67C8 -> sub_82280D90 (shape)
BANJO_RENUT_UNHOOKED_HOOK(sub_824D0488, D3DDevice_UnsetAll_hook);  // D3DDevice_UnsetAll  // katamari-nx: N&B sub_82634C10, signature 0.81 (next 0.34) // nplus: unhooked, not found yet
BANJO_RENUT_UNHOOKED_HOOK(sub_824DE3C0, D3D_ResetAllState_hook);  // D3D_ResetAllState  // katamari-nx: N&B sub_82255C98, D3D_ResetAllState: sig 0.73/0.33; the device init sub_824E18D0 calls it last // nplus: unhooked, device init only (CreateDevice is hooked)
BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_82634B90, D3D_InvalidateConstants_hook);  // D3D_InvalidateConstants  // katamari-nx: not hooked (D3D_InvalidateConstants: ambiguous (0.65 / 0.61))
BANJO_RENUT_HOOK(sub_822783B0, D3DDevice_BlockUntilIdle_hook);  // D3DDevice_BlockUntilIdle  // katamari-nx: N&B sub_82640010, signature 0.87 (next 0.63) // nplus: sub_824E0518 -> sub_822783B0 (hand)
// Through a thunk (j_D3DDevice_SetRenderTarget 0x82636EC8).
BANJO_RENUT_UNHOOKED_HOOK(sub_82636EC8, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget_Thunk  // katamari-nx: not hooked (signature too weak or ambiguous (sub_8257F318 1.00, next 1.00))
BANJO_RENUT_UNHOOKED_HOOK(sub_82636ED0, D3DDevice_GetRenderTarget_hook);  // D3DDevice_GetRenderTarget  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824B12B8 0.63, next 0.63))
BANJO_RENUT_UNHOOKED_HOOK(sub_82636F10, D3DDevice_GetDepthStencilSurface_hook);  // D3DDevice_GetDepthStencilSurface  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824B12B8 0.69, next 0.69))
BANJO_RENUT_UNHOOKED_HOOK(sub_82636E40, D3DDevice_GetStreamSource_hook);  // D3DDevice_GetStreamSource  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824DE100 0.56, next 0.42))
BANJO_RENUT_UNHOOKED_HOOK(sub_82244B18, D3DDevice_GetViewport_hook);  // D3DDevice_GetViewport  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824DC1A0 0.20, next 0.11))
BANJO_RENUT_UNHOOKED_HOOK(sub_826370E8, D3DDevice_SetClipPlane_hook);  // D3DDevice_SetClipPlane  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824D5048 0.34, next 0.33))
BANJO_RENUT_UNHOOKED_HOOK(sub_82637138, D3DDevice_SetVertexFetchConstant_hook);  // D3DDevice_SetVertexFetchConstant  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824DE100 0.38, next 0.29))
BANJO_RENUT_HOOK(sub_82260E80, D3DDevice_SetVertexShaderConstantB_hook);  // D3DDevice_SetVertexShaderConstantB  // katamari-nx: N&B sub_821F0B40, signature 0.84 (next 0.77) // nplus: sub_824D4470 -> sub_82260E80 (hand)
BANJO_RENUT_HOOK(sub_82260EE0, D3DDevice_SetPixelShaderConstantB_hook);  // D3DDevice_SetPixelShaderConstantB  // katamari-nx: N&B sub_82237208, signature 0.84 (next 0.77) // nplus: sub_824D44D0 -> sub_82260EE0 (hand)
BANJO_RENUT_UNHOOKED_HOOK(sub_826374B0, D3DDevice_SetVertexShaderConstantI_hook);  // D3DDevice_SetVertexShaderConstantI  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824D4588 0.33, next 0.33))
BANJO_RENUT_UNHOOKED_HOOK(sub_82637500, D3DDevice_SetPixelShaderConstantI_hook);  // D3DDevice_SetPixelShaderConstantI  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824D4588 0.33, next 0.33))
BANJO_RENUT_UNHOOKED_HOOK(sub_8263ED90, D3DDevice_EndIndexedVertices_hook);  // D3DDevice_EndIndexedVertices  // katamari-nx: not hooked (signature too weak or ambiguous (sub_8255A1E8 0.63, next 0.31))

// Render state getters, by their index in N&B's table (off_82E506F0, field 0).
BANJO_RENUT_HOOK(sub_8226E9E0, GetRenderStateHook<10>);  // D3DDevice_GetRenderState_ZEnable  // katamari-nx: N&B sub_82635778, render state 10 getter (state tables, this game index 10) // nplus: sub_824DC330 -> sub_8226E9E0 (render state table 10)
BANJO_RENUT_HOOK(sub_8226EA40, GetRenderStateHook<11>);  // D3DDevice_GetRenderState_ZFunc  // katamari-nx: N&B sub_826357B0, render state 11 getter (state tables, this game index 11) // nplus: sub_824DC390 -> sub_8226EA40 (render state table 11)
BANJO_RENUT_HOOK(sub_8226EA08, GetRenderStateHook<12>);  // D3DDevice_GetRenderState_ZWriteEnable  // katamari-nx: N&B sub_826357A0, render state 12 getter (state tables, this game index 12) // nplus: sub_824DC358 -> sub_8226EA08 (render state table 12)
BANJO_RENUT_HOOK(sub_8226E230, GetRenderStateHook<13>);  // D3DDevice_GetRenderState_FillMode  // katamari-nx: N&B sub_82635488, render state 13 getter (state tables, this game index 13) // nplus: sub_824DBBF8 -> sub_8226E230 (render state table 13)
BANJO_RENUT_HOOK(sub_8226E200, GetRenderStateHook<14>);  // D3DDevice_GetRenderState_CullMode  // katamari-nx: N&B sub_82635478, render state 14 getter (state tables, this game index 14) // nplus: sub_824DBBC8 -> sub_8226E200 (render state table 14)
BANJO_RENUT_HOOK(sub_8226E370, GetRenderStateHook<15>);  // D3DDevice_GetRenderState_AlphaBlendEnable  // katamari-nx: N&B sub_826354D0, render state 15 getter (state tables, this game index 15) // nplus: sub_824DBCC0 -> sub_8226E370 (render state table 15)
BANJO_RENUT_HOOK(sub_8226E708, GetRenderStateHook<16>);  // D3DDevice_GetRenderState_SeparateAlphaBlendEnable  // katamari-nx: N&B sub_82635540, render state 16 getter (state tables, this game index 16) // nplus: sub_824DC058 -> sub_8226E708 (render state table 16)
BANJO_RENUT_HOOK(sub_8226E850, GetRenderStateHook<17>);  // D3DDevice_GetRenderState_BlendFactor  // katamari-nx: N&B sub_826355E8, render state 17 getter (state tables, this game index 17) // nplus: sub_824DC1A0 -> sub_8226E850 (render state table 17)
BANJO_RENUT_HOOK(sub_8226E490, GetRenderStateHook<18>);  // D3DDevice_GetRenderState_SrcBlend  // katamari-nx: N&B sub_826354F0, render state 18 getter (state tables, this game index 18) // nplus: sub_824DBDE0 -> sub_8226E490 (render state table 18)
BANJO_RENUT_HOOK(sub_8226E520, GetRenderStateHook<19>);  // D3DDevice_GetRenderState_DestBlend  // katamari-nx: N&B sub_82635500, render state 19 getter (state tables, this game index 19) // nplus: sub_824DBE70 -> sub_8226E520 (render state table 19)
BANJO_RENUT_HOOK(sub_8226E400, GetRenderStateHook<20>);  // D3DDevice_GetRenderState_BlendOp  // katamari-nx: N&B sub_826354E0, render state 20 getter (state tables, this game index 20) // nplus: sub_824DBD50 -> sub_8226E400 (render state table 20)
BANJO_RENUT_HOOK(sub_8226E600, GetRenderStateHook<21>);  // D3DDevice_GetRenderState_SrcBlendAlpha  // katamari-nx: N&B sub_82635520, render state 21 getter (state tables, this game index 21) // nplus: sub_824DBF50 -> sub_8226E600 (render state table 21)
BANJO_RENUT_HOOK(sub_8226E670, GetRenderStateHook<22>);  // D3DDevice_GetRenderState_DestBlendAlpha  // katamari-nx: N&B sub_82635530, render state 22 getter (state tables, this game index 22) // nplus: sub_824DBFC0 -> sub_8226E670 (render state table 22)
BANJO_RENUT_HOOK(sub_8226E590, GetRenderStateHook<23>);  // D3DDevice_GetRenderState_BlendOpAlpha  // katamari-nx: N&B sub_82635510, render state 23 getter (state tables, this game index 23) // nplus: sub_824DBEE0 -> sub_8226E590 (render state table 23)
BANJO_RENUT_HOOK(sub_8226E268, GetRenderStateHook<24>);  // D3DDevice_GetRenderState_AlphaTestEnable  // katamari-nx: N&B sub_826354C0, render state 24 getter (state tables, this game index 24) // nplus: sub_824DBC30 -> sub_8226E268 (render state table 24)
BANJO_RENUT_HOOK(sub_8226E750, GetRenderStateHook<25>);  // D3DDevice_GetRenderState_AlphaRef  // katamari-nx: N&B sub_82635588, render state 25 getter (state tables, this game index 25) // nplus: sub_824DC0A0 -> sub_8226E750 (render state table 25)
BANJO_RENUT_HOOK(sub_8226E7A0, GetRenderStateHook<26>);  // D3DDevice_GetRenderState_AlphaFunc  // katamari-nx: N&B sub_826355D8, render state 26 getter (state tables, this game index 26) // nplus: sub_824DC0F0 -> sub_8226E7A0 (render state table 26)
BANJO_RENUT_HOOK(sub_8226EA88, GetRenderStateHook<27>);  // D3DDevice_GetRenderState_StencilEnable  // katamari-nx: N&B sub_826357F8, render state 27 getter (state tables, this game index 27) // nplus: sub_824DC3D8 -> sub_8226EA88 (render state table 27)
BANJO_RENUT_HOOK(sub_8226EAB8, GetRenderStateHook<28>);  // D3DDevice_GetRenderState_TwoSidedStencilMode  // katamari-nx: N&B sub_82635828, render state 28 getter (state tables, this game index 28) // nplus: sub_824DC408 -> sub_8226EAB8 (render state table 28)
BANJO_RENUT_HOOK(sub_8226EB20, GetRenderStateHook<29>);  // D3DDevice_GetRenderState_StencilFail  // katamari-nx: N&B sub_82635890, render state 29 getter (state tables, this game index 29) // nplus: sub_824DC470 -> sub_8226EB20 (render state table 29)
BANJO_RENUT_HOOK(sub_8226EB58, GetRenderStateHook<30>);  // D3DDevice_GetRenderState_StencilZFail  // katamari-nx: N&B sub_826358C8, render state 30 getter (state tables, this game index 30) // nplus: sub_824DC4A8 -> sub_8226EB58 (render state table 30)
BANJO_RENUT_HOOK(sub_8226EB88, GetRenderStateHook<31>);  // D3DDevice_GetRenderState_StencilPass  // katamari-nx: N&B sub_826358F8, render state 31 getter (state tables, this game index 31) // nplus: sub_824DC4D8 -> sub_8226EB88 (render state table 31)
BANJO_RENUT_HOOK(sub_8226EAE8, GetRenderStateHook<32>);  // D3DDevice_GetRenderState_StencilFunc  // katamari-nx: N&B sub_82635858, render state 32 getter (state tables, this game index 32) // nplus: sub_824DC438 -> sub_8226EAE8 (render state table 32)
BANJO_RENUT_HOOK(sub_8226EC80, GetRenderStateHook<33>);  // D3DDevice_GetRenderState_StencilRef  // katamari-nx: N&B sub_826359F0, render state 33 getter (state tables, this game index 33) // nplus: sub_824DC5D0 -> sub_8226EC80 (render state table 33)
BANJO_RENUT_HOOK(sub_8226ECA0, GetRenderStateHook<34>);  // D3DDevice_GetRenderState_StencilMask  // katamari-nx: N&B sub_82635A10, render state 34 getter (state tables, this game index 34) // nplus: sub_824DC5F0 -> sub_8226ECA0 (render state table 34)
BANJO_RENUT_HOOK(sub_8226ECC0, GetRenderStateHook<35>);  // D3DDevice_GetRenderState_StencilWriteMask  // katamari-nx: N&B sub_82635A30, render state 35 getter (state tables, this game index 35) // nplus: sub_824DC610 -> sub_8226ECC0 (render state table 35)
BANJO_RENUT_HOOK(sub_8226EBF0, GetRenderStateHook<36>);  // D3DDevice_GetRenderState_CCWStencilFail  // katamari-nx: N&B sub_82635960, render state 36 getter (state tables, this game index 36) // nplus: sub_824DC540 -> sub_8226EBF0 (render state table 36)
BANJO_RENUT_HOOK(sub_8226EC28, GetRenderStateHook<37>);  // D3DDevice_GetRenderState_CCWStencilZFail  // katamari-nx: N&B sub_82635998, render state 37 getter (state tables, this game index 37) // nplus: sub_824DC578 -> sub_8226EC28 (render state table 37)
BANJO_RENUT_HOOK(sub_8226EC58, GetRenderStateHook<38>);  // D3DDevice_GetRenderState_CCWStencilPass  // katamari-nx: N&B sub_826359C8, render state 38 getter (state tables, this game index 38) // nplus: sub_824DC5A8 -> sub_8226EC58 (render state table 38)
BANJO_RENUT_HOOK(sub_8226EBB8, GetRenderStateHook<39>);  // D3DDevice_GetRenderState_CCWStencilFunc  // katamari-nx: N&B sub_82635928, render state 39 getter (state tables, this game index 39) // nplus: sub_824DC508 -> sub_8226EBB8 (render state table 39)
BANJO_RENUT_HOOK(sub_8226ECE0, GetRenderStateHook<40>);  // D3DDevice_GetRenderState_CCWStencilRef  // katamari-nx: N&B sub_82635A50, render state 40 getter (state tables, this game index 40) // nplus: sub_824DC630 -> sub_8226ECE0 (render state table 40)
BANJO_RENUT_HOOK(sub_8226ED00, GetRenderStateHook<41>);  // D3DDevice_GetRenderState_CCWStencilMask  // katamari-nx: N&B sub_82635A70, render state 41 getter (state tables, this game index 41) // nplus: sub_824DC650 -> sub_8226ED00 (render state table 41)
BANJO_RENUT_HOOK(sub_8226ED20, GetRenderStateHook<42>);  // D3DDevice_GetRenderState_CCWStencilWriteMask  // katamari-nx: N&B sub_82635A90, render state 42 getter (state tables, this game index 42) // nplus: sub_824DC670 -> sub_8226ED20 (render state table 42)
BANJO_RENUT_HOOK(sub_8226ED68, GetRenderStateHook<43>);  // D3DDevice_GetRenderState_ClipPlaneEnable  // katamari-nx: N&B sub_82635A98, render state 43 getter (state tables, this game index 43) // nplus: sub_824DC6B8 -> sub_8226ED68 (render state table 43)
BANJO_RENUT_HOOK(sub_8226F0B8, GetRenderStateHook<44>);  // D3DDevice_GetRenderState_PointSize  // katamari-nx: N&B sub_82635D10, render state 44 getter (state tables, this game index 44) // nplus: sub_824DC9F0 -> sub_8226F0B8 (render state table 44)
BANJO_RENUT_HOOK(sub_8226F110, GetRenderStateHook<45>);  // D3DDevice_GetRenderState_PointSizeMin  // katamari-nx: N&B sub_82635D68, render state 45 getter (state tables, this game index 45) // nplus: sub_824DCA48 -> sub_8226F110 (render state table 45)
BANJO_RENUT_HOOK(sub_8226F060, GetRenderStateHook<46>);  // D3DDevice_GetRenderState_PointSpriteEnable  // katamari-nx: N&B sub_82635CB8, render state 46 getter (state tables, this game index 46) // nplus: sub_824DC998 -> sub_8226F060 (render state table 46)
BANJO_RENUT_HOOK(sub_8226F168, GetRenderStateHook<47>);  // D3DDevice_GetRenderState_PointSizeMax  // katamari-nx: N&B sub_82635D78, render state 47 getter (state tables, this game index 47) // nplus: sub_824DCAA0 -> sub_8226F168 (render state table 47)
BANJO_RENUT_HOOK(sub_8226EF28, GetRenderStateHook<48>);  // D3DDevice_GetRenderState_MultiSampleAntiAlias  // katamari-nx: N&B sub_82635BB8, render state 48 getter (state tables, this game index 48) // nplus: sub_824DC860 -> sub_8226EF28 (render state table 48)
BANJO_RENUT_HOOK(sub_8226EF50, GetRenderStateHook<49>);  // D3DDevice_GetRenderState_MultiSampleMask  // katamari-nx: N&B sub_82635BE0, render state 49 getter (state tables, this game index 49) // nplus: sub_824DC888 -> sub_8226EF50 (render state table 49)
BANJO_RENUT_HOOK(sub_8226ED88, GetRenderStateHook<50>);  // D3DDevice_GetRenderState_ScissorTestEnable  // katamari-nx: N&B sub_82635AB8, render state 50 getter (state tables, this game index 50) // nplus: sub_824DC6C8 -> sub_8226ED88 (render state table 50)
BANJO_RENUT_HOOK(sub_8226EE38, GetRenderStateHook<51>);  // D3DDevice_GetRenderState_SlopeScaleDepthBias  // katamari-nx: N&B sub_82635B68, render state 51 getter (state tables, this game index 51) // nplus: sub_824DC778 -> sub_8226EE38 (render state table 51)
BANJO_RENUT_HOOK(sub_8226EEF8, GetRenderStateHook<52>);  // D3DDevice_GetRenderState_DepthBias  // katamari-nx: N&B sub_82635B88, render state 52 getter (state tables, this game index 52) // nplus: sub_824DC830 -> sub_8226EEF8 (render state table 52)
BANJO_RENUT_HOOK(sub_8226EF90, GetRenderStateHook<53>);  // D3DDevice_GetRenderState_ColorWriteEnable  // katamari-nx: N&B sub_82635BE8, render state 53 getter (state tables, this game index 53) // nplus: sub_824DC8C8 -> sub_8226EF90 (render state table 53)
BANJO_RENUT_HOOK(sub_8226EFD0, GetRenderStateHook<54>);  // D3DDevice_GetRenderState_ColorWriteEnable1  // katamari-nx: N&B sub_82635C28, render state 54 getter (state tables, this game index 54) // nplus: sub_824DC908 -> sub_8226EFD0 (render state table 54)
BANJO_RENUT_HOOK(sub_8226F010, GetRenderStateHook<55>);  // D3DDevice_GetRenderState_ColorWriteEnable2  // katamari-nx: N&B sub_82635C68, render state 55 getter (state tables, this game index 55) // nplus: sub_824DC948 -> sub_8226F010 (render state table 55)
BANJO_RENUT_HOOK(sub_8226F050, GetRenderStateHook<56>);  // D3DDevice_GetRenderState_ColorWriteEnable3  // katamari-nx: N&B sub_82635CA8, render state 56 getter (state tables, this game index 56) // nplus: sub_824DC988 -> sub_8226F050 (render state table 56)
BANJO_RENUT_HOOK(sub_8226FA38, GetRenderStateHook<57>);  // D3DDevice_GetRenderState_TessellationMode  // katamari-nx: N&B sub_826363F0, render state 57 getter (state tables, this game index 57) // nplus: sub_824DD1B8 -> sub_8226FA38 (render state table 57)
BANJO_RENUT_HOOK(sub_8226F9C8, GetRenderStateHook<58>);  // D3DDevice_GetRenderState_MinTessellationLevel  // katamari-nx: N&B sub_82636380, render state 58 getter (state tables, this game index 58) // nplus: sub_824DD148 -> sub_8226F9C8 (render state table 58)
BANJO_RENUT_HOOK(sub_8226FA00, GetRenderStateHook<59>);  // D3DDevice_GetRenderState_MaxTessellationLevel  // katamari-nx: N&B sub_826363B8, render state 59 getter (state tables, this game index 59) // nplus: sub_824DD180 -> sub_8226FA00 (render state table 59)
BANJO_RENUT_HOOK(sub_8226F458, GetRenderStateHook<60>);  // D3DDevice_GetRenderState_Wrap0  // katamari-nx: N&B sub_82635FE8, render state 60 getter (state tables, this game index 60) // nplus: sub_824DCD10 -> sub_8226F458 (render state table 60)
BANJO_RENUT_HOOK(sub_8226F468, GetRenderStateHook<61>);  // D3DDevice_GetRenderState_Wrap1  // katamari-nx: N&B sub_82635FF8, render state 61 getter (state tables, this game index 61) // nplus: sub_824DCD20 -> sub_8226F468 (render state table 61)
BANJO_RENUT_HOOK(sub_8226F478, GetRenderStateHook<62>);  // D3DDevice_GetRenderState_Wrap2  // katamari-nx: N&B sub_82636008, render state 62 getter (state tables, this game index 62) // nplus: sub_824DCD30 -> sub_8226F478 (render state table 62)
BANJO_RENUT_HOOK(sub_8226F488, GetRenderStateHook<63>);  // D3DDevice_GetRenderState_Wrap3  // katamari-nx: N&B sub_82636018, render state 63 getter (state tables, this game index 63) // nplus: sub_824DCD40 -> sub_8226F488 (render state table 63)
BANJO_RENUT_HOOK(sub_8226F498, GetRenderStateHook<64>);  // D3DDevice_GetRenderState_Wrap4  // katamari-nx: N&B sub_82636028, render state 64 getter (state tables, this game index 64) // nplus: sub_824DCD50 -> sub_8226F498 (render state table 64)
BANJO_RENUT_HOOK(sub_8226F4A8, GetRenderStateHook<65>);  // D3DDevice_GetRenderState_Wrap5  // katamari-nx: N&B sub_82636038, render state 65 getter (state tables, this game index 65) // nplus: sub_824DCD60 -> sub_8226F4A8 (render state table 65)
BANJO_RENUT_HOOK(sub_8226F4B8, GetRenderStateHook<66>);  // D3DDevice_GetRenderState_Wrap6  // katamari-nx: N&B sub_82636048, render state 66 getter (state tables, this game index 66) // nplus: sub_824DCD70 -> sub_8226F4B8 (render state table 66)
BANJO_RENUT_HOOK(sub_8226F4C8, GetRenderStateHook<67>);  // D3DDevice_GetRenderState_Wrap7  // katamari-nx: N&B sub_82636058, render state 67 getter (state tables, this game index 67) // nplus: sub_824DCD80 -> sub_8226F4C8 (render state table 67)
BANJO_RENUT_HOOK(sub_8226F4D8, GetRenderStateHook<68>);  // D3DDevice_GetRenderState_Wrap8  // katamari-nx: N&B sub_82636068, render state 68 getter (state tables, this game index 68) // nplus: sub_824DCD90 -> sub_8226F4D8 (render state table 68)
BANJO_RENUT_HOOK(sub_8226F4E8, GetRenderStateHook<69>);  // D3DDevice_GetRenderState_Wrap9  // katamari-nx: N&B sub_82636078, render state 69 getter (state tables, this game index 69) // nplus: sub_824DCDA0 -> sub_8226F4E8 (render state table 69)
BANJO_RENUT_HOOK(sub_8226F4F8, GetRenderStateHook<70>);  // D3DDevice_GetRenderState_Wrap10  // katamari-nx: N&B sub_82636088, render state 70 getter (state tables, this game index 70) // nplus: sub_824DCDB0 -> sub_8226F4F8 (render state table 70)
BANJO_RENUT_HOOK(sub_8226F508, GetRenderStateHook<71>);  // D3DDevice_GetRenderState_Wrap11  // katamari-nx: N&B sub_82636098, render state 71 getter (state tables, this game index 71) // nplus: sub_824DCDC0 -> sub_8226F508 (render state table 71)
BANJO_RENUT_HOOK(sub_8226F518, GetRenderStateHook<72>);  // D3DDevice_GetRenderState_Wrap12  // katamari-nx: N&B sub_826360A8, render state 72 getter (state tables, this game index 72) // nplus: sub_824DCDD0 -> sub_8226F518 (render state table 72)
BANJO_RENUT_HOOK(sub_8226F528, GetRenderStateHook<73>);  // D3DDevice_GetRenderState_Wrap13  // katamari-nx: N&B sub_826360B8, render state 73 getter (state tables, this game index 73) // nplus: sub_824DCDE0 -> sub_8226F528 (render state table 73)
BANJO_RENUT_HOOK(sub_8226F538, GetRenderStateHook<74>);  // D3DDevice_GetRenderState_Wrap14  // katamari-nx: N&B sub_826360C8, render state 74 getter (state tables, this game index 74) // nplus: sub_824DCDF0 -> sub_8226F538 (render state table 74)
BANJO_RENUT_HOOK(sub_8226F548, GetRenderStateHook<75>);  // D3DDevice_GetRenderState_Wrap15  // katamari-nx: N&B sub_826360D8, render state 75 getter (state tables, this game index 75) // nplus: sub_824DCE00 -> sub_8226F548 (render state table 75)
BANJO_RENUT_HOOK(sub_8226F598, GetRenderStateHook<76>);  // D3DDevice_GetRenderState_ViewportEnable  // katamari-nx: N&B sub_826360E8, render state 76 getter (state tables, this game index 76) // nplus: sub_824DCE50 -> sub_8226F598 (render state table 76)
BANJO_RENUT_HOOK(sub_8226F788, GetRenderStateHook<77>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable  // katamari-nx: N&B sub_82636188, render state 77 getter (state tables, this game index 77) // nplus: sub_824DCF08 -> sub_8226F788 (render state table 77)
BANJO_RENUT_HOOK(sub_8226F838, GetRenderStateHook<78>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable1  // katamari-nx: N&B sub_82636220, render state 78 getter (state tables, this game index 78) // nplus: sub_824DCFB8 -> sub_8226F838 (render state table 78)
BANJO_RENUT_HOOK(sub_8226F8E8, GetRenderStateHook<79>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable2  // katamari-nx: N&B sub_826362B8, render state 79 getter (state tables, this game index 79) // nplus: sub_824DD068 -> sub_8226F8E8 (render state table 79)
BANJO_RENUT_HOOK(sub_8226F998, GetRenderStateHook<80>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable3  // katamari-nx: N&B sub_82636350, render state 80 getter (state tables, this game index 80) // nplus: sub_824DD118 -> sub_8226F998 (render state table 80)
BANJO_RENUT_HOOK(sub_8226FA70, GetRenderStateHook<81>);  // D3DDevice_GetRenderState_HalfPixelOffset  // katamari-nx: N&B sub_82636428, render state 81 getter (state tables, this game index 81) // nplus: sub_824DD1F0 -> sub_8226FA70 (render state table 81)
BANJO_RENUT_HOOK(sub_8226FAA0, GetRenderStateHook<82>);  // D3DDevice_GetRenderState_PrimitiveResetEnable  // katamari-nx: N&B sub_82636458, render state 82 getter (state tables, this game index 82) // nplus: sub_824DD220 -> sub_8226FAA0 (render state table 82)
BANJO_RENUT_HOOK(sub_8226FAD0, GetRenderStateHook<83>);  // D3DDevice_GetRenderState_PrimitiveResetIndex  // katamari-nx: N&B sub_82636488, render state 83 getter (state tables, this game index 83) // nplus: sub_824DD250 -> sub_8226FAD0 (render state table 83)
BANJO_RENUT_HOOK(sub_8226FAF8, GetRenderStateHook<84>);  // D3DDevice_GetRenderState_AlphaToMaskEnable  // katamari-nx: N&B sub_826364B0, render state 84 getter (state tables, this game index 84) // nplus: sub_824DD278 -> sub_8226FAF8 (render state table 84)
BANJO_RENUT_HOOK(sub_8226FB28, GetRenderStateHook<85>);  // D3DDevice_GetRenderState_AlphaToMaskOffsets  // katamari-nx: N&B sub_826364E0, render state 85 getter (state tables, this game index 85) // nplus: sub_824DD2A8 -> sub_8226FB28 (render state table 85)
BANJO_RENUT_HOOK(sub_8226FB68, GetRenderStateHook<86>);  // D3DDevice_GetRenderState_GuardBand_X  // katamari-nx: N&B sub_82636510, render state 86 getter (state tables, this game index 86) // nplus: sub_824DD2E8 -> sub_8226FB68 (render state table 86)
BANJO_RENUT_HOOK(sub_8226FB98, GetRenderStateHook<87>);  // D3DDevice_GetRenderState_GuardBand_Y  // katamari-nx: N&B sub_82636540, render state 87 getter (state tables, this game index 87) // nplus: sub_824DD318 -> sub_8226FB98 (render state table 87)
BANJO_RENUT_HOOK(sub_8226FBC0, GetRenderStateHook<88>);  // D3DDevice_GetRenderState_DiscardBand_X  // katamari-nx: N&B sub_82636568, render state 88 getter (state tables, this game index 88) // nplus: sub_824DD340 -> sub_8226FBC0 (render state table 88)
BANJO_RENUT_HOOK(sub_8226FBF0, GetRenderStateHook<89>);  // D3DDevice_GetRenderState_DiscardBand_Y  // katamari-nx: N&B sub_82636598, render state 89 getter (state tables, this game index 89) // nplus: sub_824DD370 -> sub_8226FBF0 (render state table 89)
BANJO_RENUT_HOOK(sub_8226FCB0, GetRenderStateHook<90>);  // D3DDevice_GetRenderState_HiStencilEnable  // katamari-nx: N&B sub_82636600, render state 90 getter (state tables, this game index 90) // nplus: sub_824DD430 -> sub_8226FCB0 (render state table 90)
BANJO_RENUT_HOOK(sub_8226FCE0, GetRenderStateHook<91>);  // D3DDevice_GetRenderState_HiStencilWriteEnable  // katamari-nx: N&B sub_82636630, render state 91 getter (state tables, this game index 91) // nplus: sub_824DD460 -> sub_8226FCE0 (render state table 91)
BANJO_RENUT_HOOK(sub_8226FD10, GetRenderStateHook<92>);  // D3DDevice_GetRenderState_HiStencilFunc  // katamari-nx: N&B sub_82636660, render state 92 getter (state tables, this game index 92) // nplus: sub_824DD490 -> sub_8226FD10 (render state table 92)
BANJO_RENUT_HOOK(sub_8226FD38, GetRenderStateHook<93>);  // D3DDevice_GetRenderState_HiStencilRef  // katamari-nx: N&B sub_82636688, render state 93 getter (state tables, this game index 93) // nplus: sub_824DD4B8 -> sub_8226FD38 (render state table 93)
BANJO_RENUT_HOOK(sub_8226FD48, GetRenderStateHook<94>);  // D3DDevice_GetRenderState_PresentInterval  // katamari-nx: N&B sub_82636698, render state 94 getter (state tables, this game index 94) // nplus: sub_824DD4C8 -> sub_8226FD48 (render state table 94)
BANJO_RENUT_HOOK(sub_8226FD60, GetRenderStateHook<95>);  // D3DDevice_GetRenderState_PresentImmediateThreshold  // katamari-nx: N&B sub_826366B0, render state 95 getter (state tables, this game index 95) // nplus: sub_824DD4E0 -> sub_8226FD60 (render state table 95)
BANJO_RENUT_HOOK(sub_8226FC18, GetRenderStateHook<96>);  // D3DDevice_GetRenderState_HiZEnable  // katamari-nx: N&B sub_826365C0, render state 96 getter (state tables, this game index 96) // nplus: sub_824DD398 -> sub_8226FC18 (render state table 96)
BANJO_RENUT_HOOK(sub_8226FC80, GetRenderStateHook<97>);  // D3DDevice_GetRenderState_HiZWriteEnable  // katamari-nx: N&B sub_826365D0, render state 97 getter (state tables, this game index 97) // nplus: sub_824DD400 -> sub_8226FC80 (render state table 97)
BANJO_RENUT_HOOK(sub_8226E8F8, GetRenderStateHook<98>);  // D3DDevice_GetRenderState_LastPixel  // katamari-nx: N&B sub_82635688, render state 98 getter (state tables, this game index 98) // nplus: sub_824DC248 -> sub_8226E8F8 (render state table 98)
BANJO_RENUT_HOOK(sub_8226E948, GetRenderStateHook<99>);  // D3DDevice_GetRenderState_LineWidth  // katamari-nx: N&B sub_826356D8, render state 99 getter (state tables, this game index 99) // nplus: sub_824DC298 -> sub_8226E948 (render state table 99)
BANJO_RENUT_HOOK(sub_8226E998, GetRenderStateHook<100>);  // D3DDevice_GetRenderState_Buffer2Frames  // katamari-nx: N&B sub_82635730, render state 100 getter (state tables, this game index 100) // nplus: sub_824DC2E8 -> sub_8226E998 (render state table 100)

// Sampler state getters (off_82E50BD0, field 0).
BANJO_RENUT_HOOK(sub_822704E8, GetSamplerStateHook<0>);  // D3DDevice_GetSamplerState_AddressU  // katamari-nx: N&B sub_82636C00, sampler state 0 getter (state tables, this game index 0) // nplus: sub_824DDC68 -> sub_822704E8 (sampler state table 0)
BANJO_RENUT_HOOK(sub_82270538, GetSamplerStateHook<1>);  // D3DDevice_GetSamplerState_AddressV  // katamari-nx: N&B sub_82636C18, sampler state 1 getter (state tables, this game index 1) // nplus: sub_824DDCB8 -> sub_82270538 (sampler state table 1)
BANJO_RENUT_HOOK(sub_82270588, GetSamplerStateHook<2>);  // D3DDevice_GetSamplerState_AddressW  // katamari-nx: N&B sub_82636C68, sampler state 2 getter (state tables, this game index 2) // nplus: sub_824DDD08 -> sub_82270588 (sampler state table 2)
BANJO_RENUT_HOOK(sub_82270490, GetSamplerStateHook<3>);  // D3DDevice_GetSamplerState_BorderColor  // katamari-nx: N&B sub_82636BD8, sampler state 3 getter (state tables, this game index 3) // nplus: sub_824DDC10 -> sub_82270490 (sampler state table 3)
BANJO_RENUT_HOOK(sub_8226FFC0, GetSamplerStateHook<4>);  // D3DDevice_GetSamplerState_MagFilter  // katamari-nx: N&B sub_826367A0, sampler state 4 getter (state tables, this game index 4) // nplus: sub_824DD740 -> sub_8226FFC0 (sampler state table 4)
BANJO_RENUT_HOOK(sub_8226FE30, GetSamplerStateHook<5>);  // D3DDevice_GetSamplerState_MinFilter  // katamari-nx: N&B sub_826366C0, sampler state 5 getter (state tables, this game index 5) // nplus: sub_824DD5B0 -> sub_8226FE30 (sampler state table 5)
BANJO_RENUT_HOOK(sub_822700D0, GetSamplerStateHook<6>);  // D3DDevice_GetSamplerState_MipFilter  // katamari-nx: N&B sub_82636878, sampler state 6 getter (state tables, this game index 6) // nplus: sub_824DD850 -> sub_822700D0 (sampler state table 6)
BANJO_RENUT_HOOK(sub_82270300, GetSamplerStateHook<7>);  // D3DDevice_GetSamplerState_MipMapLodBias  // katamari-nx: N&B sub_82636AB8, sampler state 7 getter (state tables, this game index 7) // nplus: sub_824DDA80 -> sub_82270300 (sampler state table 7)
BANJO_RENUT_HOOK(sub_822703B0, GetSamplerStateHook<8>);  // D3DDevice_GetSamplerState_MaxMipLevel  // katamari-nx: N&B sub_82636B00, sampler state 8 getter (state tables, this game index 8) // nplus: sub_824DDB30 -> sub_822703B0 (sampler state table 8)
BANJO_RENUT_HOOK(sub_822701E8, GetSamplerStateHook<9>);  // D3DDevice_GetSamplerState_MaxAnisotropy  // katamari-nx: N&B sub_826369A0, sampler state 9 getter (state tables, this game index 9) // nplus: sub_824DD968 -> sub_822701E8 (sampler state table 9)
BANJO_RENUT_HOOK(sub_82270080, GetSamplerStateHook<10>);  // D3DDevice_GetSamplerState_MagFilterZ  // katamari-nx: N&B sub_82636868, sampler state 10 getter (state tables, this game index 10) // nplus: sub_824DD800 -> sub_82270080 (sampler state table 10)
BANJO_RENUT_HOOK(sub_8226FEF0, GetSamplerStateHook<11>);  // D3DDevice_GetSamplerState_MinFilterZ  // katamari-nx: N&B sub_82636790, sampler state 11 getter (state tables, this game index 11) // nplus: sub_824DD670 -> sub_8226FEF0 (sampler state table 11)
BANJO_RENUT_HOOK(sub_82270170, GetSamplerStateHook<12>);  // D3DDevice_GetSamplerState_SeparateZFilterEnable  // katamari-nx: N&B sub_82636928, sampler state 12 getter (state tables, this game index 12) // nplus: sub_824DD8F0 -> sub_82270170 (sampler state table 12)
BANJO_RENUT_HOOK(sub_82270430, GetSamplerStateHook<13>);  // D3DDevice_GetSamplerState_MinMipLevel  // katamari-nx: N&B sub_82636B80, sampler state 13 getter (state tables, this game index 13) // nplus: sub_824DDBB0 -> sub_82270430 (sampler state table 13)
BANJO_RENUT_HOOK(sub_822705E0, GetSamplerStateHook<14>);  // D3DDevice_GetSamplerState_TrilinearThreshold  // katamari-nx: N&B sub_82636CC0, sampler state 14 getter (state tables, this game index 14) // nplus: sub_824DDD60 -> sub_822705E0 (sampler state table 14)
BANJO_RENUT_HOOK(sub_82270258, GetSamplerStateHook<15>);  // D3DDevice_GetSamplerState_AnisotropyBias  // katamari-nx: N&B sub_82636A10, sampler state 15 getter (state tables, this game index 15) // nplus: sub_824DD9D8 -> sub_82270258 (sampler state table 15)
BANJO_RENUT_HOOK(sub_82270638, GetSamplerStateHook<16>);  // D3DDevice_GetSamplerState_HGradientExpBias  // katamari-nx: N&B sub_82636D18, sampler state 16 getter (state tables, this game index 16) // nplus: sub_824DDDB8 -> sub_82270638 (sampler state table 16)
BANJO_RENUT_HOOK(sub_82270690, GetSamplerStateHook<17>);  // D3DDevice_GetSamplerState_VGradientExpBias  // katamari-nx: N&B sub_82636D70, sampler state 17 getter (state tables, this game index 17) // nplus: sub_824DDE10 -> sub_82270690 (sampler state table 17)
BANJO_RENUT_HOOK(sub_822706E8, GetSamplerStateHook<18>);  // D3DDevice_GetSamplerState_WhiteBorderColorW  // katamari-nx: N&B sub_82636DC8, sampler state 18 getter (state tables, this game index 18) // nplus: sub_824DDE68 -> sub_822706E8 (sampler state table 18)
BANJO_RENUT_HOOK(sub_82270748, GetSamplerStateHook<19>);  // D3DDevice_GetSamplerState_PointBorderEnable  // katamari-nx: N&B sub_82636E28, sampler state 19 getter (state tables, this game index 19) // nplus: sub_824DDEC8 -> sub_82270748 (sampler state table 19)
