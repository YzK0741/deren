// shaders/upscale.slang - THE RESOLVE: the render chain's display-referred LDR image onto the swapchain.
//
// WHY IT EXISTS: `core::render_extent()` is the RENDER chain's extent, so at `[render] render_scale < 1.0` the
// whole chain (G-buffer, HDR, `ldr`, TAA history, bloom) is created smaller while the swapchain keeps the
// output size. Nothing resolved that difference, so a scaled frame landed in the TOP-LEFT QUADRANT of the
// presented image. This pass is that resolve, and it is the frame's LAST writer whenever it runs.
//
// IT IS THE LINEAR (BILINEAR) FILTER AND NOTHING ELSE. The FSR `easu`/`easu_rcas` modes are a separate,
// later step with their own config keys: the whole point of this one is the cheapest correct answer - one
// tap of the shared linear sampler at each OUTPUT pixel centre, which is a bilinear resample of the whole
// chain. `heap_sampler_post` is the sampler that makes that true by construction (linear, clamp, one mip).
//
// THE TRANSFER IS THE ONE THING THAT CAN BE GOT WRONG HERE, so it is COPIED from fxaa.slang and from
// post.frag's composite path rather than invented: the LDR image holds DISPLAY-ENCODED values (the composite
// sets `encode_gamma = 1` when it writes that target, which applies `linear_to_srgb` by hand), and both of
// this pass's destinations are the same two FXAA already handles - an sRGB swapchain attachment, which
// encodes linear -> sRGB in HARDWARE (so it must be handed LINEAR values, or the frame is double-encoded and
// washed out: measured in post.frag as a linear 0.5 coming out at 188), and a UNORM one, which does no
// encoding at all (so the display-encoded value is what should be stored). `pc.encode_gamma` is that pair.

#include "heap_access.slang"

// THIS PASS'S OWN BLOCK, and it is deliberately NOT the post chain's (post.frag's `PostPush`): the resolve
// samples one image at one coordinate and re-encodes nothing else, so the exposure/bloom/mode/edge lanes
// would all be dead fields - and the two lanes it does need are the ones the FRAMEWORK appends (see below).
struct UpscalePush
{
    // 1 = the values this pass reads are already the display-encoded ones and the target stores them as-is
    // (a UNORM swapchain); 0 = the target is an sRGB attachment and encodes on write, so this pass must hand
    // it LINEAR values. Composed on the C++ side from the surface's format, exactly as fxaa_pass::record does.
    float encode_gamma;
    // THE HEAP INDICES AT THE END, appended by the host (`runtime::push_stage_block`) so a pass never has to
    // know the frame slot or the swapchain image (see shaders/heap_slots.glsl).
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<UpscalePush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

/// @brief sRGB -> linear, for the final write
/// @note the swapchain attachment encodes linear -> sRGB in hardware, so handing it an already encoded value
///       would double-encode (see the gamma note in post.frag); a UNORM swapchain does no such encoding, and
///       then pc.encode_gamma keeps the encoded value instead.
float3 srgb_to_linear(float3 color)
{
    float3 low = color / 12.92;
    float3 high = pow(max((color + 0.055) / 1.055, float3(0.0)), float3(2.4));
    return mix(low, high, step(float3(0.04045), color));
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_uv : TEXCOORD0) : SV_Target
{
    // ONE handle for the LDR image, built from its per-swapchain-image heap slot: the composite wrote it
    // earlier in this command buffer and upscale_pass::record is what moved it to a sampled layout. There is
    // no set to bind and no sampler to choose - the frame bound the heaps for this command buffer and
    // `heap_sampler_post` is the shared linear one (see heap_slot_constants.glsl).
    const DescriptorHandle<Texture2D<float4>> display = heap_at<Texture2D<float4>>(heap_slots_display_color + heap_image_index);

    // THE RESAMPLE: v_uv is the output pixel centre in the render image's [0,1] square, so one linear tap
    // reads the four render texels that surround it and interpolates - which is the whole "linear filter"
    // this pass is. The image is at the RENDER extent and the viewport at the OUTPUT extent, so the ratio
    // between them is the scale factor and nothing here needs to know it.
    float3 color = heap_texel(display, heap_sampler_post, v_uv).rgb;

    // Hand the result back in the encoding the TARGET expects (the same branch fxaa.slang ends with): an
    // sRGB swapchain attachment encodes linear -> sRGB in hardware, so it must receive linear values
    // (encode_gamma == 0); a UNORM swapchain does no encoding, so the display-encoded value is stored as-is.
    if (pc.encode_gamma > 0.5)
    {
        return float4(color, 1.0);
    }
    return float4(srgb_to_linear(color), 1.0);
}
