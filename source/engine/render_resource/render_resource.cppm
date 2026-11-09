// module version: 1.0.2  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file source/engine/render_resource/render_resource.cppm
 * @brief A pass's resources, described as DATA: what exists, and what each pass does with it.
 * @defgroup engine_render_resource Render Resource Descriptions
 *
 * WHY THIS MODULE EXISTS. In this renderer a pass's inputs and outputs used to BE descriptor sets - the
 * G-buffer set was the interface between the G-buffer pass, the lighting stage, the stochastic lighting chain
 * and the ray-traced shadow pass; the scene block was the substrate; each pass's private family was its I/O.
 * That interface used to be written TWICE BY HAND and kept in agreement by discipline: the layout
 * in `source/backends/vulkan/pipelines`, the descriptor writes in `source/backends/vulkan/runtime`'s `ensure_*_descriptors()`. Both drifts
 * that pair can have are already in this project's history, and both were found by the validation layer
 * rather than by review: a pool sized for four descriptors per set while the layout asked for five, and a
 * binding whose type changed without its writer noticing. One declaration, from which both were generated,
 * is what removed the pair.
 *
 * WHAT IT IS NOT, deliberately rather than unfinished:
 *
 *  * NOT a capability surface. Nothing here can reach a resource: no `core&`, no `runtime&`, no handles. It
 *    is copyable `constexpr` data with predicates over it - the opposite of a context object handed to code
 *    that must BIND things, which is the kind of object that grows as needs appear. A declaration cannot
 *    grow a capability.
 *  * NOT an allocator. Every image, buffer, view and sampler stays where it is: `deren.vulkan.core` creates them,
 *    `deren.engine.runtime` holds the pipelines. The schema says a resource EXISTS and what its scope and lifetime
 *    are; it does not say who makes it or how.
 *  * NOT Vulkan. The vocabulary is this module's enums, so the description layer is pure CPU like
 *    `deren.engine.math` and `gltf_loader` - which is what lets its invariants be tested in `ctest`, on a machine
 *    with no GPU. (The capture gate cannot run in CI at all: its references are tied to one machine's
 *    driver.) Mapping these enums onto `VkDescriptorType`/`VkShaderStageFlags` belongs to the generator,
 *    which imports Vulkan the way the pipeline builders already do.
 *
 * THE RULE THAT KEEPS THE TWO HALVES FROM DRIFTING: the SCHEMA holds what a resource IS (kind, scope,
 * lifetime, how many images the family holds); a PASS holds what it DOES with one (which binding,
 * read or write, which sampler, which stage). The only token they share is `resource_id`. A pass that
 * restates a resource's kind or scope would be a second copy of a fact - exactly the drift this module
 * exists to remove.
 *
 * dedicated BINDINGS VERSUS SHARED ONES, which the first real declaration made unavoidable: a pass with a private
 * family still reaches the frame's shared resources (a probe cell's ray needs the top level structure, the
 * material table and the texture array to resolve and shade what it finds). Such a binding is declared here as
 * USAGE - which resource, which access - and `binding_scope` says whether it is the pass's per-image
 * binding or something the frame's heap provides; the declaration is the whole of what is left of that
 * vocabulary, because every stage is heap-native now and reaches those resources through the frame's descriptor
 * heap rather than through a set (see source/backends/vulkan/core/descriptor_heap).
 */
module;

export module deren.engine.render_resource;

// The public surface is unchanged: these re-exports are the same modules the
// monolithic unit used to export, and the two partitions below are the
// implementation domains.
export import deren.utility.shared;
export import deren.engine.visibility.software;

export import :schema;
export import :passes;
