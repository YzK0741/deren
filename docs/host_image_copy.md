# Host image copy: reading an image back without a staging buffer

**STATUS: IMPLEMENTED and gated. `VK_EXT_host_image_copy` is a REQUIRED device extension here: the images this
renderer creates for itself are copied to and from host memory by the IMPLEMENTATION - an upload through
`vkCopyMemoryToImageEXT`, a read-back through `vkCopyImageToMemoryEXT` - so there is no staging buffer, no
`vkCmdCopyBufferToImage`/`vkCmdCopyImageToBuffer` and no transfer-stage barrier on those paths. A device without the
extension, without its `hostImageCopy` feature, whose entry points do not resolve, or whose layout lists lack
`GENERAL` in the direction used is REFUSED AT STARTUP with a named error - the same posture as
`VK_KHR_unified_image_layouts`. THE SCREENSHOT READ-BACK IS A DIFFERENT CASE and still uses
`vkCmdCopyImageToBuffer`: its source is a SWAPCHAIN image, whose usage the surface's `supportedUsageFlags` bounds
(see "Why the screenshot path uses the copy command - and why that is NOT a fallback").**

- The extension is a REQUIREMENT, like `VK_KHR_unified_image_layouts`: a device that cannot serve these copies is
  refused when the logical device is created, not silently routed to another mechanism. Nothing about the frames
  depends on which path runs - because there is exactly one path per direction.
- The capability is FOUR facts, not one, and all four are checked: the extension, its `hostImageCopy` feature, BOTH
  entry points resolving through `vkGetDeviceProcAddr`, and whether `VK_IMAGE_LAYOUT_GENERAL` is in the device's
  copy-source list (read-back) AND in its copy-destination list (upload) - separately, because a device may list
  GENERAL in one and not the other.
- No caller chooses a path at run time. The probe creates and reads back its OWN image, so it asks for
  `VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT` unconditionally and calls `vkCopyImageToMemoryEXT`; the log line at the end
  says so, and there is no other branch to infer.

## What the extension buys, and what it does not

`vkCopyImageToMemoryEXT` takes an `VkCopyImageToMemoryInfoEXT` naming a source image, its CURRENT layout and one or
more `VkImageToMemoryCopyEXT` regions, each with a host pointer, a subresource and an extent. The implementation
performs the copy; the app records nothing, submits nothing and owns no staging memory. It is an EXTENSION command,
so it is resolved with `vkGetDeviceProcAddr` and cached on `core` (`copy_image_to_memory`), the same way the mesh
dispatch commands are (`source/backends/vulkan/core/core.declarations.cppm`, `source/backends/vulkan/core/core.constructor.cppm`).

Two things it does NOT remove:

- The render that produced the image still has to be synchronized against. The probe records a barrier
  (`srcStage = COLOR_ATTACHMENT_OUTPUT`, `srcAccess = COLOR_ATTACHMENT_WRITE`, image stays in
  `VK_IMAGE_LAYOUT_GENERAL`) whose destination is the HOST stage / HOST_READ access, submits that command buffer and
  waits on its fence BEFORE calling the copy. The barrier is what makes the writes visible to the host; the
  submission is what makes the barrier happen. The copy call itself is then a host-side operation on an image no
  queue is using.
- The layout has to be right. `srcImageLayout` must BE the image's current layout
  (`VUID-VkCopyImageToMemoryInfo-srcImageLayout-09064`) and must appear in the device's copy-source list
  (`...-09065`); this renderer keeps every image in `VK_IMAGE_LAYOUT_GENERAL` (see
  \ref md_docs_2unified__image__layouts "Unified image layouts"), which is why GENERAL is the layout both queried
  and passed.

## The three-part capability check

`init_utils::query` decides `host_image_copy_available`, and the two halves are decided in two different places
because they come from different Vulkan structures:

1. **Extension + feature** (`vkEnumerateDeviceExtensionProperties`, then a `VkPhysicalDeviceHostImageCopyFeaturesEXT`
   in the feature `pNext` chain): `hostImageCopy` must be `VK_TRUE`. Enabling the feature and using any image with
   `VK_IMAGE_USAGE_HOST_TRANSFER_BIT` are the same fact (`VUID-VkImageCreateInfo-usage-10245`).
2. **The copy-source layouts** (`VkPhysicalDeviceHostImageCopyPropertiesEXT::pCopySrcLayouts`, in the property
   chain): the app passes an array and the count comes back as the device's total, so the query uses a fixed
   `std::array<VkImageLayout, 16>` and clamps. The only question asked of the list is "is GENERAL in it", and a list
   longer than the array would only ever make the answer a false NO (a named startup failure) rather than allow a
   copy in a layout the device does not accept. A false NO is the safe direction, which is why a fixed array is
   used instead of a two-pass query.

The check is now a REQUIREMENT, not a probe. Device creation pushes `VK_EXT_host_image_copy` unconditionally and
refuses a device whose `hostImageCopy` feature is missing, whose BOTH entry points (`vkCopyImageToMemoryEXT` and
`vkCopyMemoryToImageEXT`) do not resolve through `vkGetDeviceProcAddr`, or whose copy-source / copy-destination
layout lists do not contain `GENERAL` - each one a named startup panic, the same posture as
`VK_KHR_unified_image_layouts`. The two directions are checked against their OWN list (`pCopySrcLayouts` for the
read-back, `pCopyDstLayouts` for the upload), because a device may list GENERAL in one and not the other. No
capability flag chooses between implementations at run time.

The image the probe reads back is created with `VK_IMAGE_USAGE_HOST_TRANSFER_BIT` (and no `TRANSFER_SRC`): the
probe's image is the renderer's own, so the usage bit follows the ONE mechanism that reads it.
`VUID-VkCopyImageToMemoryInfo-srcImage-09113` requires the bit for the non-stencil aspects of the source.

## What the mapping between image and host memory is

The probe's region is the whole image, tightly packed: `memoryRowLength = 0` and `memoryImageHeight = 0`, which the
spec defines as tightly packed - the same destination bytes a `VkBufferImageCopy{bufferRowLength = 0,
bufferImageHeight = 0}` (the copy command the probe used before this change) leaves, which is what the probe's
unchanged pixel output measures (`VUID-VkImageToMemoryCopy-memoryRowLength-09101`,
`...-memoryImageHeight-09102`). The other constraints the code satisfies deliberately: `aspectMask` has one bit
(`...-aspectMask-09103`), all three extents are non-zero (`...-imageExtent-06659/60/61`), the host pointer is large
enough for the whole region (`VUID-VkImageToMemoryCopy-pHostPointer-09066`), and `flags = 0` - the
`VK_HOST_IMAGE_COPY_MEMCPY_BIT` fast path is NOT requested, because it constrains the region to the whole image with
a zero offset and buys a memcpy the tight-packing path already gives.

## Why the screenshot path uses the copy command - and why that is NOT a fallback

The screenshot path is the OTHER in-tree image read-back, and it was NOT converted. Two measured facts, one for each
reason:

1. ITS USAGE BITS ARE NOT OURS. The source is a SWAPCHAIN image, so its usage comes from
   `VkSwapchainCreateInfoKHR::imageUsage`, and `VUID-VkSwapchainCreateInfoKHR-imageUsage-01276` requires that value
   to be a SUBSET of the surface's `supportedUsageFlags`. MEASURED on this machine (raw `vulkaninfo` output in the
   batch report): that list carries SIX usages - `TRANSFER_SRC`, `TRANSFER_DST`, `SAMPLED`, `STORAGE`,
   `COLOR_ATTACHMENT`, `INPUT_ATTACHMENT` - and `HOST_TRANSFER` is NOT among them. A swapchain image therefore
   CANNOT be created with the host-copy usage here, so `vkCmdCopyImageToBuffer` is not a fallback for a missing
   device feature: it is the ONLY mechanism that can read a swapchain image on this surface. Startup records the
   fact once (`swapchain: the surface does not list VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT, so the read-back uses
   vkCmdCopyImageToBuffer`) - a record, not a branch.
2. THE COPY MUST BE RECORDED INSIDE THE FRAME, AND SINCE S2 BATCH 2 IT IS RECORDED THROUGH THE CONTRACT. Moving
   the read-back behind the contract as a frame-domain, synchronously-ordered call was implemented once and
   MEASURED TO FAIL: it ran after `submit_and_present()`, when the swapchain image is `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`
   and owned by the presentation engine, and validation reported
   `vkQueueSubmit(): ... expects VkImage ... to be in layout VK_IMAGE_LAYOUT_GENERAL--instead, current layout is
   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` for every capturing scene. THE FAILURE WAS THE TIMING, NOT THE INTERFACE, and
   the recording surface that fixes the timing now exists (`api_core::begin_commands()`, `image::format()`,
   `buffer::mapped()`, `command_list::use()` / `copy_image_to_buffer()`): the copy is recorded in the SAME slot it
   always was - `runtime::end_recording`, before the present transition - and the submitter is unchanged
   (`submit_and_present()`; this batch adds no submission and no wait). The engine touches no Vulkan handle on that
   path any more: the list, the frame image, the read-back slot and the wait all come from the contract.

## The upload path

The images this renderer creates for itself are UPLOADED through the same extension and the same policy: the four
image types that are ever created with data (`texture_2d`, `texture_2d_color`, `texture_cubemap`, `render_target`)
carry `VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT`, and `vma_allocator::host_image_upload` copies each mip with
`vkCopyMemoryToImageEXT` (one `VkMemoryToImageCopyEXT` region per mip, tightly packed, in the image's GENERAL
layout). There is no staging buffer and no copy command on that path, and no branch that could choose one: the
staging upload of BUFFERS is kept because the extension has no buffer variant (it is the only mechanism there),
and `texture_2d_staging` (LINEAR tiling, written through its host mapping) is a different KIND of image rather
than a fallback.

MEASURED, and the measurement does NOT flatter the change: the upload phase is SLOWER than the staging path it
replaced on this driver - 31.8 -> 94.3 ms for DamagedHelmet, 130.5 -> 217.8 ms for Sponza (381 uploads, 2.78 ->
1.66 GB/s), 68.3 -> 189.7 ms for the laevatain goo character. The shipped implementation submits and waits twice
per call (one barrier before the host copy, one after), which is the leading explanation and is NOT what the
staging path does (one command carrying every mip). The full reading, the pre-registered criteria and the
counter-examples are in `build-release-clang64/deren-ab/rhi/host_copy_upload_measure.md`; the change shipped
anyway because this repository's rule for a required extension is "present or a named startup failure", not
"faster or a run-time choice".

## The read-back through the contract (S2 batch 2)

`runtime::end_recording` asks the contract for the frame's list, the image the frame draws into and the backend's
host-visible slot, and records the same three steps the hand-written version recorded:

    use(image, color_attachment, transfer_source)
    copy_image_to_buffer(slot, image, region)
    use(image, transfer_source, color_attachment)

The three-step ORDER is part of the contract: `use()` takes the pair (from, to) because the backend does not record
the pass that wrote the image yet, so it cannot derive the "from"; a missing pair or a reversed one IS a wrong
barrier, and nothing can catch it for the caller. What can be checked is that the pair lands on the recipes this
renderer already shipped, and that check is the SHADOW GATE (plan 8.3):

- COMPILE TIME: `source/backends/vulkan/core/core.api_core.cpp` derives each barrier from the pair and `static_assert`s equality
  with `constant_init`'s `color_attachment_to_transfer_transition` / `transfer_to_color_attachment_transition` over
  all sixteen fields (stage/access masks, both layouts, both queue family indices, the image slot and the four
  subresource fields). A mismatch is a build failure.
- RUN TIME: the same two barriers are dumped once per pair, so both sides are in the run's own log. MEASURED on the
  `deferred` scenario:

        rhi shadow gate (A5) use(0, 1) image=0x50000000005
        rhi shadow gate (A5)   derived: sType=0x3b9f9492 pNext=0x0 srcStage=0x400 srcAccess=0x100 dstStage=0x1000
          dstAccess=0x800 oldLayout=0x1 newLayout=0x1 srcQFI=0xffffffff dstQFI=0xffffffff image=0x50000000005
          aspect=0x1 mip=0+1 layer=0+1
        rhi shadow gate (A5)   recipe : sType=0x3b9f9492 pNext=0x0 srcStage=0x400 srcAccess=0x100 dstStage=0x1000
          dstAccess=0x800 oldLayout=0x1 newLayout=0x1 srcQFI=0xffffffff dstQFI=0xffffffff image=0x0 aspect=0x1
          mip=0+1 layer=0+1
        (and the mirror pair use(1, 0): srcStage=0x1000 srcAccess=0x800 dstStage=0x400 dstAccess=0x100)

  The ONLY field that differs is `image`: the recipe constants carry `VK_NULL_HANDLE` because they are templates,
  and the recording site fills in the frame image. Every other field is equal, which is what the compile-time
  assertion proves without the image being read.
- The capture itself stays byte-identical: `check_render.ps1 -Full` reports all THIRTEEN capturing scenarios at the
  frozen pre-change hashes (deferred `972A31EC5FF55C87`, ..., laevatain_old_chain `190EB09D3E9FDCDA`). Its summary
  says `passed 2 / changed 12`, which is the STALE LOCAL BASELINE and not a pixel change - the hashes are the
  criterion, exactly as `build-release-clang64/deren-ab/rhi/host_copy_upload_measure.md` records for the batch
  before this one. `laevatain_no_sidecar` is the pre-existing red (`chars/laevatain.glb` is not in the tree).
- NO validation line in any capturing scenario, and a window RESIZE is survivable: measured by resizing the GLFW
  window while the renderer ran, the surface went out of date, the swapchain was rebuilt (1080x960 -> 1028x694), the
  read-back slot was re-created at the new size, and the capture that followed is a valid 1028x694 PNG.

## Evidence

MEASURED on this machine:

- The device reports the capability (`vulkaninfo`): `VK_EXT_host_image_copy : extension revision 1` and
  `hostImageCopy = true`, and the startup line reports both layout lists' answer.
- The three graphics probes read their pixel back through the HOST IMAGE COPY and report the same rgba values as
  before (the probe's own proof: the selected material slot reads white 255,255,255,255 and the deliberately wrong
  slot does not).
- The surface CANNOT host-copy a swapchain image: `supportedUsageFlags` has six entries and no `HOST_TRANSFER`
  (VUID-VkSwapchainCreateInfoKHR-imageUsage-01276), so the screenshot keeps `vkCmdCopyImageToBuffer` as its sole
  mechanism.
- A tier-1 (frame-domain, synchronously ordered) read-back of that image was implemented and FAILED under
  validation: 13 capturing scenes report the `PRESENT_SRC_KHR` / `GENERAL` mismatch above, which is why the
  screenshot read-back was left where it was (see "What is NOT established" and the batch report for the raw
  lines).
- No frame changed: `scripts/windows/check_render.ps1 -Full` reports the same 13 render hashes byte for byte, the
  screenshot PNG is unchanged (the read-back did not move), and `ctest` is 14/14 (including `test_docs`, which is
  why `docs/host_image_copy.md` is in the `Doxyfile` INPUT list).

## What is NOT established

- NO PERFORMANCE CLAIM. Removing a staging buffer and a copy command from a diagnostic probe is not a speedup
  anybody can see; the extension is here because it is the correct mechanism for the copies this renderer performs,
  not because a number moved. No timing was taken.
- No device WITHOUT the extension was run. The policy is a NAMED STARTUP REFUSAL (extension, feature, both entry
  points, and GENERAL in each direction's layout list), and its failure path is exercised by temporarily forcing the
  capability check false and observing the refusal - not by hardware.
- The swapchain read-back is not host-copyable on this surface, and a surface that DID list `HOST_TRANSFER` would
  still get the copy command from this batch: converting it is a separate change with its own measurement.
- No validation-layer or synchronization-validation run was made for the host copy; the barrier chain is reasoned
  from the synchronization chapter plus the wait that precedes the call.
