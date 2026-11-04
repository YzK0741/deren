// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:object
// ============================================================================
module;

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

export module deren.promise.rhi:object;

import deren.utility.shared;

export namespace deren::promise::rhi {

    /// Concrete implementation identity. Names must have static storage duration.
    /// A name is not proof of device ownership or object lifetime.
    class object {
    public:
        std::string_view const type;

    protected:
        constexpr explicit object(std::string_view const name) noexcept
            : type(name) {
        }
        ~object() = default; // Destruction stays on each interface's release path.
    };

    /// WHAT AN RHI HANDLE IS, for the containers below: every template here is constrained to a type
    /// that inherits `object`, so a non-RHI type cannot be wrapped in one (there would be no `release()`
    /// for the container to call, and the constraint says so at compile time).
    template <typename candidate>
    concept object_type = std::derived_from<candidate, object>;

    /// ONE REFERENCE TO AN RHI OBJECT - the factories' return type.
    ///
    /// Move-only RAII and nothing else: the destructor calls `release()`, the contract's "drop the
    /// caller's ONE reference" (not `delete`: the backend answers reference counting itself and a shared
    /// resource may outlive the call). Deliberately not a standard-library pointer - its drop is a
    /// contract verb, and spelling that as a deleter type turns one verb into two spellings.
    template <typename rhi_object>
    class unique_object {
    public:
        unique_object() noexcept = default;

        /// take the reference @p instance carries (null is allowed: a factory refusal answers with one)
        unique_object(rhi_object* const instance) noexcept
            : instance{instance} {
            static_assert(object_type<rhi_object>, "unique_object manages only a contract object handle");
        }

        unique_object(unique_object&& other) noexcept
            : instance{other.instance} {
            other.instance = nullptr;
        }

        unique_object& operator=(unique_object&& other) noexcept {
            if (this != &other) {
                this->reset();
                this->instance = other.instance;
                other.instance = nullptr;
            }
            return *this;
        }

        unique_object(unique_object const&) = delete;
        unique_object& operator=(unique_object const&) = delete;

        ~unique_object() noexcept {
            this->reset();
        }

        /// DROP THE REFERENCE, ONCE (idempotent): a shared resource may outlive this call
        void reset() noexcept {
            static_assert(object_type<rhi_object>, "unique_object manages only a contract object handle");
            if (this->instance != nullptr) {
                this->instance->release();
                this->instance = nullptr;
            }
        }

        /// give up the reference WITHOUT dropping it (the caller takes over the `release()`)
        [[nodiscard]] rhi_object* release() noexcept {
            rhi_object* const released = this->instance;
            this->instance = nullptr;
            return released;
        }

        void swap(unique_object& other) noexcept {
            rhi_object* const temporary = this->instance;
            this->instance = other.instance;
            other.instance = temporary;
        }

        [[nodiscard]] rhi_object* get() const noexcept {
            return this->instance;
        }
        [[nodiscard]] rhi_object* operator->() const noexcept {
            return this->instance;
        }
        [[nodiscard]] rhi_object& operator*() const noexcept {
            return *this->instance;
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return this->instance != nullptr;
        }

        friend bool operator==(unique_object const& value, std::nullptr_t) noexcept {
            return value.instance == nullptr;
        }
        friend bool operator!=(unique_object const& value, std::nullptr_t) noexcept {
            return value.instance != nullptr;
        }
        friend bool operator==(unique_object const& left, unique_object const& right) noexcept {
            return left.instance == right.instance;
        }
        friend bool operator!=(unique_object const& left, unique_object const& right) noexcept {
            return left.instance != right.instance;
        }

    private:
        rhi_object* instance = nullptr;
    };

    namespace detail {
        /// THE ONE CONTROL BLOCK `shared_object` and its nested `weak` count: the strong count owns the
        /// single `release()` the factory reference stands for, `drop` is what that release runs (null =
        /// a BORROWED handle: it owns no reference and drops nothing), and the weak count keeps the block
        /// alive for the weak handles.
        struct shared_object_control final {
            shared_object_control(void (*drop_instance)(void*) noexcept, void* const own_instance) noexcept
                : owned{own_instance}
                , drop{drop_instance} {
            }
            /// the object whose reference the block owns; NOT the alias target (an aliasing handle may
            /// point at a different object than the one the control block drops)
            void* owned = nullptr;
            std::atomic<std::uint32_t> strong{1};
            /// One implicit weak reference belongs to the strong group until object destruction
            /// finishes. External weak releases can never delete a block still used by the drop.
            std::atomic<std::uint32_t> weak{1};
            void (*drop)(void*) noexcept = nullptr;
        };
    } // namespace detail

    /// SHARED OWNERSHIP OF ONE RHI REFERENCE - the contract's counterpart of a shared pointer.
    ///
    /// Copies share ONE control block, so the factory's single reference is dropped EXACTLY ONCE when the
    /// last copy goes (`release()`, as in `unique_object`). `borrow()` makes a non-owning handle that keeps
    /// a backend-owned pointer shareable but drops nothing (the frame-view case); the aliasing constructor
    /// binds a borrowed pointer to an ENGINE owner's lifetime (the engine's lease pattern).
    template <typename rhi_object>
    class shared_object {
    public:
        /// the non-owning observer of a shared handle (see `lock()`)
        class weak;

        shared_object() noexcept = default;
        shared_object(std::nullptr_t) noexcept {
        }

        /// take ownership of the one reference @p instance carries
        explicit shared_object(rhi_object* const instance) noexcept
            : instance{instance} {
            static_assert(object_type<rhi_object>, "shared_object manages only a contract object handle");
            if (instance != nullptr) {
                this->control = new detail::shared_object_control{&shared_object::drop_reference, instance};
            }
        }

        /// A BORROWED handle: shares the pointer with copies and drops no reference. A null control block is
        /// the non-owning spelling: copies copy the pointer, destruction clears it, `weak()` is empty and no
        /// allocation happens. The owner must keep @p instance alive for the whole use (standard §6.2.2).
        static shared_object borrow(rhi_object& instance) noexcept {
            shared_object borrowed;
            borrowed.instance = &instance;
            return borrowed;
        }

        shared_object(unique_object<rhi_object>&& owner) noexcept
            : shared_object(owner.release()) {
        }

        /// a handle to a DERIVED object as a handle to the interface: the SAME control block is shared,
        /// so the one reference is still dropped exactly once.
        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        shared_object(shared_object<derived_object> const& other) noexcept
            : instance{static_cast<rhi_object*>(other.get())}
            , control{other.control}
            , alias_owner{other.alias_owner} {
            this->acquire();
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        shared_object(shared_object<derived_object>&& other) noexcept
            : instance{static_cast<rhi_object*>(other.get())}
            , control{other.control}
            , alias_owner{std::move(other.alias_owner)} {
            other.instance = nullptr;
            other.control = nullptr;
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        shared_object(unique_object<derived_object>&& owner) noexcept
            : shared_object(static_cast<rhi_object*>(owner.release())) {
        }

        /// AN ALIAS TO A BORROWED RHI POINTER tied to an engine owner's lifetime (the engine lease shape):
        /// the owner keeps the resource alive and this handle owns no RHI reference of its own.
        /// an ENGINE-OWNED handle (a test's std::make_shared<fake_root>, say) adopted as an alias: the
        /// shared pointer's own reference is the lifetime, so no RHI release runs.
        template <typename owner_type>
            requires std::derived_from<owner_type, rhi_object>
        shared_object(std::shared_ptr<owner_type> owner) noexcept
            : instance{owner.get()}
            , alias_owner{std::move(owner)} {
        }

        template <typename owner_type>
        shared_object(std::shared_ptr<owner_type> owner, rhi_object* const pointer) noexcept
            : instance{pointer}
            , alias_owner{std::move(owner)} {
        }

        /// the same alias with a CONTRACT-OWNED root: shares that handle's control block (so the root
        /// outlives the borrowed pointer) without dropping an extra reference of its own.
        template <typename owner_object>
            requires std::derived_from<owner_object, object>
        shared_object(shared_object<owner_object> const& owner, rhi_object* const pointer) noexcept
            : instance{pointer}
            , control{owner.control} {
            this->acquire();
        }

        shared_object(shared_object const& other) noexcept
            : instance{other.instance}
            , control{other.control}
            , alias_owner{other.alias_owner} {
            this->acquire();
        }

        shared_object(shared_object&& other) noexcept
            : instance{other.instance}
            , control{other.control}
            , alias_owner{std::move(other.alias_owner)} {
            other.instance = nullptr;
            other.control = nullptr;
        }

        shared_object& operator=(shared_object const& other) noexcept {
            if (this != &other) {
                shared_object copy{other};
                this->swap(copy);
            }
            return *this;
        }

        shared_object& operator=(shared_object&& other) noexcept {
            if (this != &other) {
                this->reset();
                this->instance = other.instance;
                this->control = other.control;
                this->alias_owner = std::move(other.alias_owner);
                other.instance = nullptr;
                other.control = nullptr;
            }
            return *this;
        }

        ~shared_object() noexcept {
            this->reset();
        }

        /// drop this copy's share (the last copy performs the one `release()`)
        void reset() noexcept {
            if (this->control == nullptr) {
                this->instance = nullptr;
                this->alias_owner.reset();
                return;
            }
            if (this->control->strong.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                if (this->control->drop != nullptr) {
                    this->control->drop(this->control->owned);
                }
                if (this->control->weak.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    delete this->control;
                }
            }
            this->instance = nullptr;
            this->control = nullptr;
        }

        void swap(shared_object& other) noexcept {
            rhi_object* const other_instance = other.instance;
            other.instance = this->instance;
            this->instance = other_instance;
            detail::shared_object_control* const other_control = other.control;
            other.control = this->control;
            this->control = other_control;
            this->alias_owner.swap(other.alias_owner);
        }

        [[nodiscard]] rhi_object* get() const noexcept {
            return this->instance;
        }
        [[nodiscard]] rhi_object* operator->() const noexcept {
            return this->instance;
        }
        [[nodiscard]] rhi_object& operator*() const noexcept {
            return *this->instance;
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return this->instance != nullptr;
        }

        /// how many shared copies hold the reference (the aliased owner's count for an alias); 0 = empty
        [[nodiscard]] std::uint32_t use_count() const noexcept {
            if (this->control != nullptr) {
                return this->control->strong.load(std::memory_order_acquire);
            }
            return this->alias_owner != nullptr ? static_cast<std::uint32_t>(this->alias_owner.use_count()) : 0u;
        }
        /// True for an owning reference or lifetime alias, false for borrow()/empty.
        /// An alias owner must actually retain its target; this cannot inspect arbitrary owner types.
        [[nodiscard]] bool owns_lifetime() const noexcept {
            return instance != nullptr && ((control != nullptr && control->drop != nullptr) || alias_owner != nullptr);
        }
        [[nodiscard]] bool unique() const noexcept {
            return this->use_count() == 1u;
        }

        /// whether @p other shares THIS handle's control block (the std::shared_ptr::owner_before identity)
        [[nodiscard]] bool shares_control(shared_object const& other) const noexcept {
            return this->control != nullptr ? this->control == other.control
                                            : this->alias_owner.get() == other.alias_owner.get();
        }

        friend bool operator==(shared_object const& value, std::nullptr_t) noexcept {
            return value.instance == nullptr;
        }
        friend bool operator!=(shared_object const& value, std::nullptr_t) noexcept {
            return value.instance != nullptr;
        }
        friend bool operator==(shared_object const& left, shared_object const& right) noexcept {
            return left.instance == right.instance;
        }
        friend bool operator!=(shared_object const& left, shared_object const& right) noexcept {
            return left.instance != right.instance;
        }

    private:
        template <typename>
        friend class shared_object;

        void acquire() noexcept {
            if (this->control != nullptr) {
                this->control->strong.fetch_add(1, std::memory_order_relaxed);
            }
        }

        static void drop_reference(void* const instance) noexcept {
            static_cast<rhi_object*>(instance)->release();
        }

        rhi_object* instance = nullptr;
        detail::shared_object_control* control = nullptr;
        std::shared_ptr<void const> alias_owner;
    };

    /// THE WEAK SIDE of a shared RHI handle: observes without owning and locks into a shared handle while a
    /// strong reference is still alive. Nested in `shared_object` because it exists for exactly this family.
    template <typename rhi_object>
    class shared_object<rhi_object>::weak {
    public:
        weak() noexcept = default;

        weak(shared_object<rhi_object> const& owner) noexcept
            : instance{owner.instance}
            , control{owner.control} {
            if (this->control != nullptr) {
                this->control->weak.fetch_add(1, std::memory_order_relaxed);
            }
        }

        weak(weak const& other) noexcept
            : instance{other.instance}
            , control{other.control} {
            if (this->control != nullptr) {
                this->control->weak.fetch_add(1, std::memory_order_relaxed);
            }
        }

        weak(weak&& other) noexcept
            : instance{other.instance}
            , control{other.control} {
            other.instance = nullptr;
            other.control = nullptr;
        }

        weak& operator=(shared_object<rhi_object> const& owner) noexcept {
            weak copy{owner};
            this->swap(copy);
            return *this;
        }
        weak& operator=(weak const& other) noexcept {
            if (this != &other) {
                weak copy{other};
                this->swap(copy);
            }
            return *this;
        }
        weak& operator=(weak&& other) noexcept {
            if (this != &other) {
                this->release_control();
                this->instance = other.instance;
                this->control = other.control;
                other.instance = nullptr;
                other.control = nullptr;
            }
            return *this;
        }

        ~weak() noexcept {
            this->release_control();
        }

        /// a shared handle while at least one strong reference is alive, empty otherwise
        [[nodiscard]] shared_object<rhi_object> lock() const noexcept {
            if (this->control == nullptr) {
                return {};
            }
            std::uint32_t count = this->control->strong.load(std::memory_order_acquire);
            while (count != 0) {
                if (this->control->strong.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    shared_object<rhi_object> locked;
                    locked.instance = this->instance;
                    locked.control = this->control;
                    return locked;
                }
            }
            return {};
        }

        [[nodiscard]] bool expired() const noexcept {
            return this->control == nullptr || this->control->strong.load(std::memory_order_acquire) == 0;
        }

        [[nodiscard]] std::uint32_t use_count() const noexcept {
            return this->control != nullptr ? this->control->strong.load(std::memory_order_acquire) : 0u;
        }

        void reset() noexcept {
            this->release_control();
        }

        void swap(weak& other) noexcept {
            rhi_object* const other_instance = other.instance;
            other.instance = this->instance;
            this->instance = other_instance;
            detail::shared_object_control* const other_control = other.control;
            other.control = this->control;
            this->control = other_control;
        }

    private:
        void release_control() noexcept {
            if (this->control != nullptr) {
                if (this->control->weak.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    delete this->control;
                }
                this->control = nullptr;
                this->instance = nullptr;
            }
        }

        rhi_object* instance = nullptr;
        detail::shared_object_control* control = nullptr;
    };

    /// THE CONCRETE-IDENTITY MECHANISM, SHARED BY EVERY BACKEND: a handle's `type` is the concrete
    /// implementation name (ABI 30), the backend OWNS the name constants, and the validate-then-cast
    /// operation lives HERE so a second backend cannot reinvent it (a D3D12 backend validates its own
    /// names with the same two calls).
    ///
    /// WHY THE GRANULARITY IS CONCRETE, NOT THE INTERFACE: one interface has several concrete
    /// implementations with different layouts behind it (`buffer` is owned and, separately, the
    /// backend's borrowed read-back slot; `image` is owned and the borrowed swapchain image), so the
    /// `static_cast` a true answer permits is defined only for the ONE layout the name names.
    ///
    /// A FALSE ANSWER IS A CALLER CONDITION at the function's entrance: a handle this backend did not
    /// hand out, which the contract answers `rhi::error::invalid_argument` (or `nullptr` from a
    /// factory). It is never a panic. The panic belongs to `do_concrete()`, which runs only after the
    /// provenance has already been established - see its note.

    /// @brief whether @p value is the concrete implementation @p name names
    [[nodiscard]] constexpr bool is_concrete(object const& value, std::string_view const name) noexcept {
        return value.type == name;
    }

    /// @brief the concrete implementation behind @p value, as its own type
    ///
    /// PANICS ON A MISMATCH, AND THAT IS DELIBERATE: by the time this is called the provenance has
    /// already been established (registry membership, an object-identity comparison, ...), so a name
    /// that does not match is the BACKEND'S own bookkeeping being inconsistent - a bug in the code
    /// holding the invariant, which is what `deren::utility::panic` exists for ("a failure the code
    /// EXPECTS travels as a value; a violated invariant terminates"). Callers that instead have a
    /// handle which may legitimately be foreign use `is_concrete()` and answer
    /// `rhi::error::invalid_argument`; they never route a container's caller bug through this cast.
    ///
    /// @param value the handle to cast; MUST already be proven to belong to the calling backend
    /// @param name the concrete name the cast requires, taken from that backend's own constants
    /// @param where the CALLER's site, captured by the default argument; the panic reports it
    /// @tparam concrete_t the backend's concrete adapter type; spelled in snake_case because it is a
    ///         local name for the type, not a conventional `T`
    template <typename concrete_t>
    [[nodiscard]] concrete_t const* do_concrete(object const& value, std::string_view const name,
                                                std::source_location const where = std::source_location::current()) noexcept {
        if (!is_concrete(value, name)) {
            deren::utility::panic(deren::utility::vessel::format(
                                      "rhi: do_concrete: a handle named \"{}\" is not the \"{}\" this cast requires - "
                                      "provenance was already established, so the name and the registry disagree",
                                      value.type, name),
                                  where);
        }
        return static_cast<concrete_t const*>(&value);
    }

    /// the mutable spelling, for the sites that record INTO the handle (a command buffer, a build target)
    template <typename concrete_t>
    [[nodiscard]] concrete_t* do_concrete(object& value, std::string_view const name,
                                          std::source_location const where = std::source_location::current()) noexcept {
        if (!is_concrete(value, name)) {
            deren::utility::panic(deren::utility::vessel::format(
                                      "rhi: do_concrete: a handle named \"{}\" is not the \"{}\" this cast requires - "
                                      "provenance was already established, so the name and the registry disagree",
                                      value.type, name),
                                  where);
        }
        return static_cast<concrete_t*>(&value);
    }

} // namespace deren::promise::rhi
