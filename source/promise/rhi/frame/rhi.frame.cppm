// -*- C++ -*-
// module: deren.promise.rhi:frame
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module deren.promise.rhi:frame;
export import deren.utility.shared;

import :contract;
import :recording;
import :resource;

export namespace deren::promise::rhi {

    struct swapchain : object {

        explicit swapchain(std::string_view const name) noexcept
            : object(name) {
        }
        virtual ~swapchain() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;

        // ---- the presentation verbs (abi 14) --------------------------------------------------
        // The swapchain was created once at startup and is rebuilt IN PLACE - never replaced - so
        // the verbs below are APPENDED to this interface rather than new methods on `api_core`: a
        // rebuild is the swapchain's own behaviour (the caller supplies nothing - the backend
        // re-derives the extent from the window it owns, which is the one place the current size
        // lives).
        /// Rebuild the swapchain and its images for the window's current size.
        /// `ok` = a new generation was built; `not_ready` = the window reports a zero extent (a
        /// minimized or not-yet-sized window), nothing was built, and the caller retries later -
        /// a deferred recreate is a STATE, not a failure, and `error` carries it as one; real
        /// build failures are the backend's named startup-style panics (the same rule the
        /// constructor's build path follows).
        [[nodiscard]] virtual error recreate() = 0;

        /// The extent of the swapchain's images, in pixels - what presentation shows. The caller
        /// scales it to its own render resolution (a render scale is a RENDERER decision, not a
        /// presentation fact, so the contract does not pre-multiply it).
        [[nodiscard]] virtual image_extent extent() const noexcept = 0;

        /// Presentation format available before acquiring an image (ABI 28).
        [[nodiscard]] virtual image_format format() const noexcept = 0;
    };
    /// What starting a frame hands back.
    ///
    /// §3.3 fixes the name and the fact that it is a value, not a handle; the fields the
    /// frame-in-flight ring and the timeline machinery need are §6.5's, so this is the
    /// smallest version that is still worth returning.
    struct submit_info {
        std::uint32_t frame_index = 0; ///< index in the frame-in-flight ring
        std::uint32_t image_index = 0; ///< the swapchain image this frame draws into
    };

    /// What opening one frame decided, AND WHY - the frame face's answer type (abi 13).
    ///
    /// BY VALUE, THEREFORE FROZEN: the same rule `submit_info` and `error_info` live under - a
    /// by-value POD's size is part of its calling convention, so any field addition moves
    /// `abi_version`. This type is the minimal repair of a measured information loss: today's
    /// `frame_begin()` squashes OUT_OF_DATE and every other failure into the same zeroed
    /// `submit_info`, so the caller cannot tell a resized window from a dead device, and
    /// `wait_frame_slot()` drops `vkWaitSemaphores`' result entirely. Both arrive here now:
    ///
    /// 1. `result.code == error::ok` => `frame` names the opened frame. ANYTHING else => `frame` is
    ///    zeroed and MUST NOT be used - the readable "zero means no frame" of today, but now the
    ///    caller also knows WHY (out_of_date => rebuild and skip; device_lost => fatal; ...).
    /// 2. `result` may come from EITHER half of the open - the slot's timeline wait, or the image
    ///    acquire. `result.message` names the step and `result.native_code` carries the raw VkResult;
    ///    `where` is the backend's failure point. This is where `wait_frame_slot()`'s dropped
    ///    `vkWaitSemaphores` result comes back.
    struct frame_open_info {
        submit_info frame = {}; ///< the opened frame; zeroed and invalid unless result.code == ok
        error_info result = {}; ///< ok, or the failure of whichever step refused
    };

    /// The frame-in-flight ring depth this ABI is written for (abi 15's follow-up, ③-D/E batch).
    ///
    /// DATA, NOT A VTABLE: an `inline constexpr`, so adding it cannot renumber `abi_version` (the same
    /// rule every appended `error` value follows). It exists because a compile-time number is what the
    /// engine needs for its per-slot C++ (arrays sized by the ring), which `frame_walker::slot_count()`
    /// - the RUNTIME truth, and the only authority - cannot provide. The two are therefore pinned to
    /// each other rather than trusted: the backend `static_assert`s its own ring against this constant,
    /// and the engine checks `slot_count()` against it once at startup and PANICS on a mismatch (a
    /// backend whose ring is a different size must fail loudly, never overrun a per-slot array).
    inline constexpr std::uint32_t max_frames_in_flight = 2u;

    /// THE PER-IMAGE ARRAY BOUND A BACKEND PROMISES (③-D/E item A1): the swapchain hands out no more than
    /// this many images, so a caller may size its per-image arrays (targets, views, per-image flags) as a
    /// C++ array of this many slots.
    ///
    /// DATA, NOT A VTABLE - the `max_frames_in_flight` rule, for the same reason: an integer whose only job
    /// is to size an array must not renumber `abi_version`. The alternative was a `swapchain::image_count()`
    /// virtual (abi 17), which buys a runtime number the caller would then have to allocate around.
    ///
    /// IT IS A PROMISE THE BACKEND KEEPS, NOT A GUESS ABOUT THE DRIVER: a driver that reports MORE images
    /// than this makes the backend REFUSE THE CREATION LOUDLY - never clamp, never resize past the
    /// constant, because the caller's arrays are already sized by it. The backend `static_assert`s this
    /// against its own frozen bound, and the ENGINE asserts an index before it uses one (a panic, never a
    /// silent out-of-range read). A backend that cannot keep this promise is what the abi-17
    /// `swapchain::image_count()` route is for.
    inline constexpr std::uint32_t max_swapchain_images = 4u;

    /// The frame ring's cursor, as a BORROWED VIEW - the `command_buffer` shape, not the owned-handle
    /// one: a view carries no reference, so `unique_object` must not wrap it (its release panics).
    /// `api_core` hands out the same object every call.
    ///
    /// WHY THE RING IS A CONTRACT FACE AT ALL: the cursor's one home is the backend's own frame
    /// state - the engine today keeps a SECOND copy of it (`current_frame` field reads, the
    /// `MAX_FRAMES_IN_FLIGHT` constant) plus three raw verbs (wait, acquire, advance), which is the
    /// "two sources of truth" shape this face exists to end. The engine borrows the view and walks;
    /// the backend stays the only authority. `position()` and a successful `wait_and_acquire()`'s
    /// `frame.frame_index` answer the SAME slot - the consistency is asserted, not hoped for.
    ///
    /// THE ORDER INSIDE `wait_and_acquire()` IS LOAD-BEARING (it is today's frame prologue, fused):
    /// read the cursor without advancing; wait that slot's timeline (value 0 = never submitted =>
    /// nothing to wait for); LATCH the slot's last completed frame's GPU timings - here, between the
    /// wait and the acquire, exactly where the engine's `collect_gpu_timings` sits today, so a frame
    /// that dies on OUT_OF_DATE is still collected (behaviour unchanged); acquire the next image;
    /// report. `walk_to_next()` advances after present, and it advances ONE step - it does not
    /// promise "the frame is over" (`end_frame` would), only that the ring moved.
    struct frame_walker {
        virtual ~frame_walker() noexcept = default;

        /// how many slots the ring has (today's `MAX_FRAMES_IN_FLIGHT` reads).
        [[nodiscard]] virtual std::uint32_t slot_count() const noexcept = 0;

        /// the slot THIS frame is being recorded into (today's `current_frame` field reads). Read-only:
        /// the cursor moves in `walk_to_next()`, never in a read.
        [[nodiscard]] virtual std::uint32_t position() const noexcept = 0;

        /// wait this slot's timeline, latch its previous frame's GPU timings, acquire the next image,
        /// and report the decision. See the type's note for the order and the zero-frame rule.
        [[nodiscard]] virtual frame_open_info wait_and_acquire() = 0;

        /// advance the ring one slot - the present-side close of the frame (today's `to_next_frame()`).
        virtual void walk_to_next() noexcept = 0;
    };

    /// The GPU timing report of the LAST COMPLETED frame, as a BORROWED VIEW (no `release()`, same
    /// rule as `frame_walker`).
    ///
    /// The REPORT, not the recording: marks are written by the backend while a frame is recorded
    /// (a mark's semantic name rides the mark itself - the engine's static text, held as a pointer,
    /// the same rule as `window_title`), and the backend latches a slot's finished frame when its
    /// timeline is waited. Reading is then pure host-side work the CALLER orders: after
    /// `wait_and_acquire()` answers, this face describes the frame that JUST completed.
    ///
    /// A "stage" is the interval one mark OPENS: stage i runs from mark i to mark i + 1 and is named
    /// by the name mark i carried. The last mark closes the frame and opens nothing - asking for it
    /// is `not_ready`. Durations are NANOSECONDS (the backend's timestamp period converts; on the
    /// device this backend ships on it is 1 ns/tick with 64 valid bits, so the integer is lossless);
    /// the display layer converts to milliseconds.
    ///
    /// THE ERRORS ARE THE ERROR MECHANISM WORKING: `unsupported` = this device or configuration
    /// cannot timestamp (and `stage_count()` is then always 0); `invalid_argument` = an index at or
    /// past `stage_count()`; `device_lost` = the timestamp read-back failed; `not_ready` = no marks
    /// latched yet, or the index names the frame's last mark, which opens no stage.
    struct gpu_profiler {
        virtual ~gpu_profiler() noexcept = default;

        /// how many marks the last completed frame wrote (0 = nothing latched, or no timing support).
        [[nodiscard]] virtual std::uint32_t stage_count() const noexcept = 0;

        /// one stage's name and duration. `name` / `duration_ns` are caller-provided stores and may be
        /// nullptr individually (ask only what you need); the name is the static text the mark carried,
        /// valid as long as the process runs (the backend is never unloaded - invariant 4).
        [[nodiscard]] virtual error get_stage_info(std::uint32_t index, std::string_view* name, std::uint64_t* duration_ns) const noexcept = 0;
    };

    /// Borrowed resource creation interface. Empty products mean creation was refused/failed.
    /// Descriptors are borrowed until return; products retain their existing ownership rules.
    /// Shared immutable payload. Other aliases must not mutate bytes during the resource lifetime.
    /// Borrowed frame/presentation interface, also present for offscreen frames (present is unsupported).
    /// Walker/profiler/surface are stable borrowed views: never release/delete them.
    struct frame_context {
        frame_context() noexcept = default;
        virtual ~frame_context() noexcept = default;
        frame_context(frame_context const&) = delete;
        frame_context& operator=(frame_context const&) = delete;
        /// Current frame's primary, or null outside recording. Borrowed until frame submission.
        [[nodiscard]] virtual command_buffer* begin_commands() = 0;
        /// Last acquired image; null before first acquire, valid through submit until the next acquire.
        [[nodiscard]] virtual image* frame_image() noexcept = 0;
        /// Host-visible slot used by the frame copy; completion/host visibility is the caller's responsibility.
        [[nodiscard]] virtual buffer* frame_readback_buffer() noexcept = 0;
        /// Legacy acquisition result; prefer walk_frames()->wait_and_acquire() for full error information.
        [[nodiscard]] virtual submit_info frame_begin() = 0;
        [[nodiscard]] virtual error present() = 0;
        [[nodiscard]] virtual frame_walker* walk_frames() noexcept = 0;
        [[nodiscard]] virtual gpu_profiler* profiler() noexcept = 0;
        [[nodiscard]] virtual swapchain* frame_swapchain() noexcept = 0;
        /// Submit the current frame using backend-owned acquire/presentation state.
        [[nodiscard]] virtual error submit() = 0;
    };

} // namespace deren::promise::rhi
