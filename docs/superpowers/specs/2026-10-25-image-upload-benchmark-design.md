# Image upload size benchmark design

User requested size-dependent Host Image Copy versus staging measurements before choosing a hybrid
strategy. This is an isolated measurement tool; existing production upload policy remains unchanged.

Use the real backend device/graphics queue and an independent VMA allocator with identical GPU-only
image allocation policy. Reproduce current host path (two command-buffer layout transitions, each
submitted/waited, surrounding vkCopyMemoryToImageEXT). Compare staging buffer/memcpy/flush followed by
one submission containing barriers+vkCmdCopyBufferToImage. Include native host layout transition plus
host copy, and a staging variant retaining the host usage bit: four routes in total. The compatible
staging variant separates copy-path effects from image-usage/allocation differences.

Measure image allocation, staging preparation, host copy call, queue submit and fence wait separately;
end-to-end allocation+upload excludes payload generation, content hashing, decode, destruction and
readback. All methods finish in GENERAL and use the same source/mip/layer layout. Alternate method order
and distinguish fresh staging allocation from warmed persistent staging. Validate exact byte readback
outside timed regions, both with and without validation layers. No default strategy change based on
one machine. No claim that host-image-copy API itself is intrinsically slower: current path has extra
synchronization. Single-image latency is not asynchronous/batched streaming throughput or draw performance.

Cover RGBA8 16..4096, BC7/mips, RGBA16F, cube/mips and NPOT cases, with warmups and repeated paired samples.
Save raw CSV+device metadata and a summary table with median/p95. Keep live resources bounded by one
image, a persistent staging buffer, an optional fresh staging buffer and a persistent readback buffer.
VMA can retain backing blocks. Logical staging capacity is not allocated/resident memory; no peak-VRAM
claim follows from it. No source texture files are saved.

Ruling after initial validation: include host_native (vkTransitionImageLayoutEXT UNDEFINED->GENERAL
then host copy, zero queue submissions during upload). The Vulkan spec explicitly makes the host writes
visible on the next queue submission. This separates avoidable synchronization overhead from host
copy itself before recommending staging or a threshold. Reference:
https://docs.vulkan.org/refpages/latest/refpages/source/vkCopyMemoryToImage.html
