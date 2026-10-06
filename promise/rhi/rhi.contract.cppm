// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:contract
//
// The two pieces every other partition of deren.promise.rhi needs: the ABI number the
// backend and the engine compare, and the error enum that turns a failure into a
// return value instead of an exception (RHI plan v4, §4.1 item 3 and §4.2).
//
// `deren::promise::rhi::abi_version` is the compile-time constant whose twin is the
// backend's `deren_abi_version()`. Plan §8.1 row 4 makes the comparison a tested
// check rather than a convention: the loader resolves the symbol and the test
// asserts the returned number equals this constant. The value is part of the C
// ABI: it moves only when the shape of `api_core` or of a tier-2 ability changes
// in a way an older engine could not survive.
//
// Nothing here allocates, throws, or names a container. Both sides of the boundary
// compile this partition (plan §4.1 item 1); neither exports a symbol for it.
// ============================================================================
module;

#include <cstddef>
#include <cstdint>
#include <source_location>
#include <string_view>
#include <type_traits>

export module deren.promise.rhi:contract;

/**
 * @file promise/rhi/rhi.contract.cppm
 * @brief the ABI number the backend and the engine compare, and the `error` enum every promise entry
 *        point reports through.
 * @ingroup promise
 *
 * Both are part of the C ABI rather than of any implementation: `abi_version` is the number
 * `deren_abi_version()` returns and `deren_make_api_core()` refuses to build against, and `error`
 * is what a failure travels in because an exception must not cross the boundary (§4.2).
 * Deliberately the smallest partition in the contract: both sides compile it, so anything added here
 * is added to both compilations at once.
 */

export namespace deren::promise::rhi {

    /// The C ABI number of this build of the contract.
    ///
    /// `deren_abi_version()` in the backend returns it and `deren_make_api_core()`
    /// refuses any other value, which is the "engine and backend disagree" failure
    /// path the skeleton keeps testable on purpose (plan §4.2).
    ///
    /// 1 -> 2 in the recording-surface batch: SIX virtuals were added to EXISTING tier-1 types
    /// (`api_core::frame_image`/`frame_readback_buffer`, `image::format`, `buffer::mapped`,
    /// `command_list::use`/`copy_image_to_buffer`), which shifts the vtable every caller reaches
    /// through. Appending `error` values or adding a NEW interface does not move this number; a
    /// virtual on an existing type does. FROM S2 ON THIS IS STRICTER: the DLL ABI promise is live
    /// then, so a renumbering breaks binaries in the field rather than only recompiling this tree.
    ///
    /// 2 -> 3 in the one-creation-structure batch: `deren_make_api_core()` grew the parameter it was
    /// always described as taking - a `deren::promise::rhi::create_info const*` - and the contract's
    /// creation structure is now the ONLY one (the backend's `vulkan::core_create_info` twin and the
    /// `to_backend_create_info()` translation between them are gone). No vtable moved, but the C
    /// entry's SIGNATURE did: an engine built for 2 calls that symbol with two arguments and would
    /// have its third read from whatever the stack held, which is exactly the failure this number
    /// exists to catch before anything is allocated (the mismatch is refused with `error::abi_mismatch`).
    ///
    /// 3 -> 4 in the owned-handle batch: `release()` landed as a pure virtual on SEVEN existing tier-1
    /// types (`buffer`, `image`, `sampler`, `shader`, `pipeline`, `swapchain`, `query`), which shifts the
    /// vtable every caller reaches through - the same reason 1 -> 2 moved the number. It is an ADD to
    /// existing types, not a new interface, so it is exactly the case the rule above names.
    ///
    /// A RENAME IN PLACE DOES NOT MOVE IT, and that is worth spelling out where the number is defined:
    /// the entry was briefly called `destroy()`, was renamed to `release()` to say "this drops ONE
    /// reference; the resource may be shared/reused and therefore outlive the call", and the number
    /// stayed 4. The SLOT is unchanged (same position, same order), only the symbol name is, so a build
    /// compiled against one spelling and a build compiled against the other remain binary compatible -
    /// the rule is about the SHAPE of the vtables, not about their spelling.
    /// 4 -> 5 in the addressable-buffer batch: `acceleration_structure_address` MOVED from the
    /// `device_address` ability to `ray_tracing`, where the only ability that can produce its operand
    /// lives. Both tier-2 abilities change shape (one loses a virtual, the other gains one), which is
    /// exactly the case the rule above names: an engine built for 4 would dispatch those slots
    /// differently. The move is what lets `device_address` be announced at all - while it carried an
    /// operand no backend could produce yet, announcing it was forbidden by the ability's own rule.
    /// 5 -> 6 when `vulkan_escape` gained `native_buffer`: a virtual APPENDED to an existing tier-2
    /// ability. Appending keeps the slots already there where they were, but the vtable every caller
    /// reaches through is a different shape, which is the case this number exists for. It is the call
    /// that lets an escaping pass use a contract buffer's raw handle instead of the allocator's detail
    /// map, i.e. one more way for the engine to stop reaching into the backend.
    /// NOT a bump - recorded so the decision outlives the session: the extension.cppm revision
    /// (codex/dynamic-link-v3 @ ab14ca6) deleted the documented `deren_ext_<ability>_v1` C
    /// function-table requirement, leaving `query_extension()` as the only ability entrance. What
    /// was removed is a PROSE requirement (the gate walks abilities() bits against objects, not
    /// symbols), not a virtual, an entry point, or any type the ABI numbers exist to compare - no
    /// vtable shape moves, so the rule above gives no reason for a bump.
    /// 6 -> 7 in the image face (DYNAMIC_LINK_V2.md §17's decided design, landed):
    /// `image` GAINED `make_view()` and with it the owned `image_view` interface, `image_desc` /
    /// `sampler_desc` / `image_view_desc` became defined types, `image_format` grew the creation
    /// formats and the `depth` role, and `vulkan_escape` grew `native_image` / `native_image_view` /
    /// `native_sampler`. Every change APPENDS to an existing vtable or adds a new interface, which is
    /// the case the number exists for: an engine compiled for 6 dispatches those slots differently.
    /// The `depth` format value is pinned at 0x7FFFFFFF so later named formats can append without
    /// moving it.
    /// 7 -> 8 in the pipeline face: `shader_desc` / `pipeline_desc` became defined types (the blend
    /// vocabulary is the FOUR MODES the survey found behind the raw blend constants, the depth
    /// compare the two the renderer uses), the factories gain real implementations, and
    /// `vulkan_escape` grew `native_pipeline` / `native_shader_module`. Append-only; the number
    /// exists because the vtables the engine dispatches through changed shape.
    /// 8 -> 9: vulkan_escape appends heap_ready/heap_properties. The latter returns a
    /// value-only POD mirror, never the backend's heap_limits reference; the former
    /// reports actual heap readiness. Appending changes the vtable shape.
    /// 9 -> 10: vulkan_escape appends native image/buffer heap descriptor writes.
    /// Native image view data is a value-only POD; pNext is deliberately unsupported.
    /// 10 -> 11: vulkan_escape appends native heap bind/push and binding snapshots.
    /// Native primary/secondary command buffers are borrowed from this device's recording domain.
    /// 11 -> 12: all RHI objects carry a sealed interface identity; heap services move
    /// from Vulkan escape to descriptor_heap with tagged, size-checked request structures.
    /// NOT a bump - recorded so the decision outlives the session (the error-mechanism batch):
    /// `error` grew six appended values (13-18), and `error_zone` / `zone_of()` / `graphics_api` /
    /// `error_info` were added as NEW types and a constexpr free function. No virtual moved, no
    /// signature changed shape, and the factory's `error*` out-parameter is still an `error*` - an
    /// appended enum value is data an old engine simply receives as `operation_failed`-class news it
    /// has never seen, which the rule above gives no reason to number. The `error*` -> `error_info*`
    /// entry-signature change that DOES move the number arrives with the frame face (abi 12 -> 13),
    /// in one step with the new tier-1 types.
    /// 12 -> 13 in the frame face: `api_core` APPENDED `walk_frames()` / `profiler()` - a virtual on
    /// an existing tier-1 type shifts every vtable that reaches through it, which is exactly the case
    /// the rule names - and `frame_walker` / `gpu_profiler` / `frame_open_info` are new tier-1 types.
    /// The factory entry's out-parameter changed `error*` -> `error_info*` in the SAME bump (one
    /// renumbering for one batch): the C entry's SIGNATURE is what the number protects, and an engine
    /// built for 12 calling 13's symbol would read a 40-byte diagnostic where it wrote a 4-byte enum.
    /// 13 -> 14 in the frame verbs' completion (the boundary batch, one renumbering for one batch):
    /// `api_core` appended `submit(command_list&)` and `frame_swapchain()`, `present()` changed
    /// `void` -> `error` (a presentation that failed silently was the information loss the error
    /// mechanism exists to end), `command_list` appended the GPU timing recording verbs
    /// (`begin_gpu_timing()` / `mark_gpu_timing(index, name)`), and `swapchain` appended
    /// `recreate()` / `extent()` - every one an append or a return-type change on a tier-1 vtable,
    /// which is the case the number exists for. The swapchain verbs live on the SWAPCHAIN, not on
    /// the context, because a rebuild is the swapchain's own behaviour; the extent is the
    /// presentation's fact and the render scale stays the caller's own decision.
    inline constexpr std::uint32_t abi_version = 14u;

    /// Why a promise entry point could not do what it was asked.
    ///
    /// An exception must not cross the boundary (plan §4.2), so every failure a
    /// backend can report travels as one of these through an out-parameter.
    ///
    /// `abi_mismatch` = 7 is not a system error number: it is the code the minimal
    /// use case measured in plan §10.3 reports, and it is kept here so that the
    /// refusal stays observable from the engine side.
    ///
    /// Values 13-18 are APPENDED (2026-10-05, the error-mechanism batch) - never renumber, never
    /// reuse: an old engine compiled against 0-12 receives a code it has never seen and still has to
    /// route it somewhere, which appending keeps possible. Each value names the class of failure the
    /// CALLER acts on, because that is what a code crossing a boundary is for; the raw API-specific
    /// number travels beside it in `error_info::native_code`.
    ///
    /// `suboptimal` (Vulkan's present/acquire state) deliberately has NO value here: it is a state,
    /// not a failure, and the same VkResult means "carry on" at acquire and "rebuild" at present -
    /// the translation is the CALL SITE's decision, not the enum's.
    enum class error : std::uint32_t {
        ok = 0,                     ///< the call did what it was asked
        abi_mismatch = 7,           ///< the caller's abi_version is not the backend's
        unsupported = 8,            ///< the backend has no mechanism that can serve THIS resource/format
        invalid_argument = 9,       ///< the region does not fit the image, or the destination is too small
        not_ready = 10,             ///< no frame is in flight, or the frame that drew it is not done
        device_lost = 11,           ///< the device refused the submission/copy (VkResult failure)
        operation_failed = 12,      ///< underlying operation failed without a more precise error channel
        out_of_date = 13,           ///< the swapchain/target expired: rebuild it and retry, skipping this frame
        surface_lost = 14,          ///< the window/surface is gone: rebuild it; if the window closed, exit the loop
        timeout = 15,               ///< a wait or a query ran out of time: retryable, or degrade and carry on
        out_of_device_memory = 16,  ///< device memory exhausted: RECOVERABLE - free resources and retry
        out_of_host_memory = 17,    ///< host memory exhausted: usually fatal
        initialization_failed = 18, ///< creation failed and no code above names the cause: report and exit
    };

    /// Which subsystem an `error` belongs to - as the CONTRACT derives it, never as a stored field.
    ///
    /// A zone stored next to the code would be a second source of truth able to contradict it (the
    /// same reason `validate_structure` answers a code rather than filling in a struct): inside the
    /// contract the zone is a pure function of the code, so `zone_of` is the only way it exists.
    /// Zones that genuinely carry independent information - the application's own graphics / asset /
    /// config / platform / internal vocabulary - live at the application layer, where the caller knows
    /// WHICH subystem asked for the work; the contract does not carry that field for them.
    enum class error_zone : std::uint8_t {
        api = 0,      ///< the device / window / frame pipeline refused or deferred the call
        resource = 1, ///< the backend has no mechanism that can serve the request
        argument = 2, ///< the caller passed something that cannot work as given
        internal = 3, ///< the backend's own machinery refused, mismatched or broke
    };

    /// The zone of @p code - total over the enum, so a new value forces its way through this switch
    /// (-Werror) and cannot silently inherit another zone.
    [[nodiscard]] constexpr error_zone zone_of(error const code) noexcept {
        switch (code) {
        case error::unsupported:
            return error_zone::resource;
        case error::invalid_argument:
            return error_zone::argument;
        // The api zone is "the device/window/frame state refused or deferred the call": a lost
        // device, an expired swapchain, a gone surface, a wait that ran out, memory the device or
        // host no longer has - and `not_ready`, which is the same family (the frame pipeline is
        // simply not there yet). The caller's move in every one of these is retry / rebuild /
        // degrade, not "fix the arguments" and not "report a backend bug".
        case error::not_ready:
        case error::device_lost:
        case error::out_of_date:
        case error::surface_lost:
        case error::timeout:
        case error::out_of_device_memory:
        case error::out_of_host_memory:
            return error_zone::api;
        // internal = the machinery, not the request: `ok` (a successful call's zone when a caller
        // wants one), the ABI handshake's own refusal, a catch-all failure and a creation that
        // failed under no heading above.
        case error::ok:
        case error::abi_mismatch:
        case error::operation_failed:
        case error::initialization_failed:
            return error_zone::internal;
        }
        return error_zone::internal; // unreachable for a valid enumerator; -Wswitch still guards above
    }

    /// Which graphics API produced a diagnostic. Append-only, same rule as `error` itself.
    enum class graphics_api : std::uint8_t {
        unknown = 0,
        vulkan = 1,
    };

    /// The decision AND the diagnosis of one failure, as the backend's failure point reported it.
    ///
    /// FROZEN BY RULE - the contract's by-value PODs are frozen (`submit_info` set the precedent):
    /// this struct is returned and carried BY VALUE, so its size is part of the calling convention an
    /// old engine and a new backend would disagree on. Any field addition moves `abi_version` - it is
    /// the same reason the factory's out-parameter changes `error*` -> `error_info*` only together
    /// with that bump. (An out-parameter structure the CALLER allocates could carry a `struct_size`
    /// header and grow safely; a by-value one cannot.)
    ///
    /// Three premises this definition rests on, each one a reason the next edit should think twice:
    /// 1. BOTH HALVES COMPILE WITH THE SAME TOOLCHAIN. `std::source_location`'s layout is
    ///    implementation-defined; the engine and a backend DLL compare layouts, not definitions.
    ///    That was already implied by `vstd` (the trimmed std module both halves share) and by the
    ///    BMI exchange; stating it here makes it a clause. The static_asserts below pin the layout
    ///    clang64 compiles, so a toolchain change fails loudly here instead of at a call site.
    /// 2. `message` and `where`'s text point into the BACKEND's static storage - which outlives the
    ///    call only because invariant 4 holds: the backend DLL is loaded once and NEVER unloaded
    ///    (the loader's `detach()` exists for exactly this). A backend that unloads hands its host
    ///    dangling text.
    /// 3. `native_code` is SIGNED on purpose: the Vulkan codes it carries are negative
    ///    (VK_ERROR_OUT_OF_DATE_KHR = -1000001004), and that number stored in an unsigned field is a
    ///    different number by the time a human reads it.
    struct error_info {
        error code = error::ok;                   ///< the decision layer: what the caller acts on
        graphics_api api = graphics_api::unknown; ///< who produced the diagnostic
        std::int32_t native_code = 0;             ///< the raw API code (signed; see premise 3)
        std::string_view message = {};            ///< the backend's static text, possibly empty
        std::source_location where = {};          ///< captured at the BACKEND's failure point, not the engine's call site
    };

    // The pinned layout (clang64: clang 22.x, libc++, x86-64). `code` 4 bytes, `api` 1 + 3 padding,
    // `native_code` 4, then `message` (a char const* + a size_t) and `where` land on their 8-byte
    // alignments. `where` is 8 bytes, not 24: libc++'s std::source_location holds ONE pointer to a
    // static __impl record (file/function/line/column), which also means the text it names lives in
    // static storage - premise 2's guarantee, measured rather than assumed. Trivially copyable is the
    // DESIGN constraint - the struct crosses a C-shaped boundary by value - and standard layout is
    // what makes offsetof below defined at all.
    static_assert(std::is_trivially_copyable_v<error_info>, "error_info crosses the boundary by value; it must stay trivially copyable");
    static_assert(std::is_standard_layout_v<error_info>, "error_info's pinned offsets below need standard layout");
    static_assert(sizeof(error_info) == 40, "error_info changed size - by-value PODs are frozen: moving abi_version is part of the change");
    static_assert(offsetof(error_info, code) == 0, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, api) == 4, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, native_code) == 8, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, message) == 16, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, where) == 32, "error_info's pinned layout moved");

    /// 接口身份由RHI定义，不能由后端重解释；数值只追加，不复用。
    enum class interface_type : std::uint32_t {
        unknown = 0,
        api_core = 1,
        buffer = 2,
        image = 3,
        image_view = 4,
        sampler = 5,
        shader = 6,
        pipeline = 7,
        swapchain = 8,
        query = 9,
        command_list = 10,
        device_address = 0x100,
        descriptor_heap = 0x101,
        mesh_shader = 0x102,
        ray_tracing = 0x103,
        host_image_copy = 0x104,
        vulkan_escape = 0x105,
    };

    /// 只用于接口识别/调试，不是设备归属、对象存活或具体实现布局的证明。
    class object {
    public:
        [[nodiscard]] constexpr interface_type type() const noexcept {
            return this->type_;
        }

    protected:
        constexpr explicit object(interface_type const type) noexcept
            : type_(type) {
        }
        ~object() = default; // 禁止经共同根delete；保持各接口既有的release/析构路径。

    private:
        interface_type const type_;
    };

    enum class structure_type : std::uint32_t {
        unknown = 0,
        heap_image_write = 1,
        heap_buffer_write = 2,
        heap_bind = 3,
        heap_push = 4,
        vulkan_heap_image = 0x10000,
        vulkan_command_buffer = 0x10001,
    };

    /// next只借用到同步调用结束；当前只接受明确支持的一层后端参数，不静默丢链。
    struct structure_header {
        structure_type s_type = structure_type::unknown;
        std::uint32_t struct_size = sizeof(structure_header);
        structure_header const* next = nullptr;
    };

    [[nodiscard]] constexpr error validate_structure(structure_header const& header, structure_type const expected,
                                                     std::size_t const required_size, bool const allow_next = false) noexcept {
        if (header.struct_size < sizeof(structure_header) || header.struct_size < required_size || header.s_type != expected) {
            return error::invalid_argument;
        }
        if (!allow_next && header.next != nullptr) {
            return error::unsupported;
        }
        return error::ok;
    }

} // namespace deren::promise::rhi
