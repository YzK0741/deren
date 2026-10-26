module;
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
export module deren.engine.pipelined_residency;

export namespace deren::engine::pipelined_residency {
    /// Candidates prepared for display. Three slots are the first-version bound (design section
    /// "scheduling"); the scheduler never grows them, so a slow prepare cannot turn into an
    /// unbounded queue.
    inline constexpr std::size_t slot_capacity = 3;

    enum class phase : std::uint8_t {
        empty,
        visibility,
        feedback_ready,
        uploading,
        renderable,
        shading,
        complete,
        retiring,
    };

    struct request {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
    };

    struct candidate {
        std::uint64_t request_id = 0;
        std::uint64_t generation = 0;
        phase state = phase::empty;
        bool stale = false;
    };

    struct present_result {
        bool displayed = false;
        bool reset_history = false;
    };

    /// Pure-CPU state machine for prepared/display candidates. It deliberately carries no GPU
    /// handles: the runtime owns commands, feedback, textures and colors per slot, while this class
    /// is the bounded ordering/lifetime policy that can be tested without a device.
    class scheduler {
    public:
        explicit scheduler(std::size_t slots = slot_capacity)
            : candidates(slots) {
        }

        [[nodiscard]] std::size_t slot_count() const noexcept {
            return candidates.size();
        }
        [[nodiscard]] bool has_pending() const noexcept {
            return pending.has_value();
        }
        [[nodiscard]] std::optional<request> pending_request() const noexcept {
            return pending;
        }

        /// Record the latest request. Older/duplicate IDs are ignored; a newer request replaces the
        /// single unscheduled mailbox entry. Returns false when the request is not newer.
        bool request_now(request value) noexcept {
            if (value.id == 0 || value.generation == 0)
                return false;
            if (latest_request >= value.id)
                return false;
            latest_request = value.id;
            if (current_generation != value.generation) {
                // A scene generation change invalidates every candidate from the previous scene.
                // Active work is not cancelled here; it becomes stale and is retired after completion.
                current_generation = value.generation;
                for (auto& slot : candidates) {
                    if (slot.state != phase::empty && slot.generation != current_generation)
                        slot.stale = true;
                }
            }
            pending = value;
            return true;
        }

        /// Move the pending request into a free slot and run visibility. With no free slot the
        /// mailbox stays full and the caller simply present/reuses the last complete color.
        [[nodiscard]] std::optional<std::size_t> start_visibility() noexcept {
            if (!pending)
                return std::nullopt;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (candidates[i].state != phase::empty)
                    continue;
                candidates[i] = candidate{.request_id = pending->id, .generation = pending->generation, .state = phase::visibility};
                pending.reset();
                return i;
            }
            return std::nullopt;
        }

        [[nodiscard]] bool feedback_ready(std::size_t slot) noexcept {
            if (!valid(slot) || candidates[slot].state != phase::visibility)
                return false;
            candidates[slot].state = phase::feedback_ready;
            return true;
        }

        /// At most one upload batch is in flight; choose the oldest feedback-ready candidate.
        [[nodiscard]] std::optional<std::size_t> start_upload() noexcept {
            for (auto const& slot : candidates)
                if (slot.state == phase::uploading)
                    return std::nullopt;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (candidates[i].state != phase::feedback_ready)
                    continue;
                candidates[i].state = phase::uploading;
                return i;
            }
            return std::nullopt;
        }

        [[nodiscard]] bool finish_upload(std::size_t slot, bool success) noexcept {
            if (!valid(slot) || candidates[slot].state != phase::uploading)
                return false;
            candidates[slot].state = success ? phase::renderable : phase::retiring;
            return true;
        }

        /// Shading is serialized on the display path in Phase 1: one candidate at a time.
        [[nodiscard]] std::optional<std::size_t> start_shading() noexcept {
            for (auto const& slot : candidates)
                if (slot.state == phase::shading)
                    return std::nullopt;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (candidates[i].state != phase::renderable)
                    continue;
                candidates[i].state = phase::shading;
                return i;
            }
            return std::nullopt;
        }

        [[nodiscard]] bool finish_shading(std::size_t slot) noexcept {
            if (!valid(slot) || candidates[slot].state != phase::shading)
                return false;
            candidates[slot].state = phase::complete;
            return true;
        }

        /// The newest complete, non-stale candidate. A failed or retired candidate is never chosen.
        [[nodiscard]] std::optional<std::size_t> latest_complete() const noexcept {
            std::optional<std::size_t> best;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                auto const& slot = candidates[i];
                if (slot.state != phase::complete || slot.stale)
                    continue;
                if (!best || slot.request_id > candidates[*best].request_id)
                    best = i;
            }
            return best;
        }

        /// Present a complete candidate. The presentation ID is monotonic: an older/late candidate
        /// cannot move the displayed frame backwards. Repeated presentation of the same candidate
        /// reports displayed=false and does not advance history.
        [[nodiscard]] present_result present(std::size_t slot) noexcept {
            if (!valid(slot) || candidates[slot].state != phase::complete || candidates[slot].stale)
                return {};
            if (candidates[slot].request_id <= presented_id)
                return {};
            auto const& value = candidates[slot];
            bool const reset_history = value.request_id != presented_id || value.generation != presented_generation;
            presented_id = value.request_id;
            presented_generation = value.generation;
            return {.displayed = true, .reset_history = reset_history};
        }

        /// Retire a complete or failed candidate. In-flight work must finish first; retiring a
        /// complete slot releases it for the next visibility request.
        [[nodiscard]] bool retire(std::size_t slot) noexcept {
            if (!valid(slot))
                return false;
            auto& value = candidates[slot];
            if (value.state != phase::complete && value.state != phase::retiring)
                return false;
            value = {};
            return true;
        }

        [[nodiscard]] phase state_of(std::size_t slot) const noexcept {
            return valid(slot) ? candidates[slot].state : phase::empty;
        }
        [[nodiscard]] bool stale(std::size_t slot) const noexcept {
            return valid(slot) && candidates[slot].stale;
        }
        [[nodiscard]] std::uint64_t presented() const noexcept {
            return presented_id;
        }
        [[nodiscard]] std::uint64_t generation() const noexcept {
            return current_generation;
        }

    private:
        [[nodiscard]] bool valid(std::size_t slot) const noexcept {
            return slot < candidates.size();
        }

        std::vector<candidate> candidates;
        std::optional<request> pending;
        std::uint64_t latest_request = 0;
        std::uint64_t current_generation = 0;
        std::uint64_t presented_id = 0;
        std::uint64_t presented_generation = 0;
    };
} // namespace deren::engine::pipelined_residency
