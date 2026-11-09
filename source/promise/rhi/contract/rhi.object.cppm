// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:object
// ============================================================================
module;
#include <cstddef>
#include <cstdint>

export module deren.promise.rhi:object;
export import deren.vstd;

import deren.utility.shared;

export namespace deren::promise::rhi {

    /// Concrete implementation identity, reported by `get_tag()` from the concrete type's static `tag`.
    /// A tag is not proof of device retention or object lifetime.
    class object {
    public:
        [[nodiscard]] virtual std::string_view get_tag() const noexcept = 0;

        /// Release the caller's one reference to this object.
        virtual void release() noexcept = 0;

    protected:
        constexpr object() noexcept = default;
        virtual ~object() = default; // Destruction stays on each interface's release path.
    };

    /// A TYPE that carries its concrete identity as a static tag. The concept constrains the type,
    /// not a runtime value: `object::get_tag()` supplies the runtime answer for an opaque handle.
    template <typename candidate>
    concept tagged = requires { { candidate::tag } -> std::convertible_to<std::string_view>; };

    /// WHAT AN RHI HANDLE IS, for the containers below: every template here is constrained to a type
    /// that inherits `object`, so a non-RHI type cannot be wrapped in one (there would be no `release()`
    /// for the container to call, and the constraint says so at compile time).
    template <typename candidate>
    concept object_type = std::derived_from<candidate, object>;

    /// ONE REFERENCE TO AN RHI OBJECT - the factories' return type.
    ///
    /// Move-only RAII and nothing else: the destructor calls `release()`, the contract's "release the
    /// caller's ONE reference" (not `delete`: the backend answers reference counting itself and a shared
    /// resource may outlive the call). Deliberately not a standard-library pointer - its release is a
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

        unique_object(unique_object const&) = delete ("unique_object is move-only: transfer retention with std::move");
        unique_object& operator=(unique_object const&) = delete ("unique_object is move-only: assign from an rvalue, not a copy");

        ~unique_object() noexcept {
            this->reset();
        }

        /// RELEASE THE REFERENCE, ONCE (idempotent): a shared resource may outlive this call
        void reset() noexcept {
            static_assert(object_type<rhi_object>, "unique_object manages only a contract object handle");
            if (this->instance != nullptr) {
                this->instance->release();
                this->instance = nullptr;
            }
        }

        /// give up the reference WITHOUT releasing it (the caller takes over the `release()`)
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
        /// THE ONE CONTROL BLOCK `shared_object` and `weak_object` count: the strong count holds the
        /// single `release()` the factory reference stands for, the `release` callback performs that
        /// contract call (null = a non-holding handle: it holds no reference and releases nothing), and the
        /// weak count keeps the block alive for the weak handles.
        struct shared_object_control final {
            shared_object_control(void (*release_instance)(void*) noexcept, void* const instance_handle) noexcept
                : instance{instance_handle}
                , release{release_instance} {
            }
            /// the object whose reference the block holds; NOT the alias target (an aliasing handle may
            /// point at a different object than the one the control block releases)
            void* instance = nullptr;
            std::atomic<std::uint32_t> strong{1};
            /// One implicit weak reference belongs to the strong group until object destruction
            /// finishes. External weak releases can never delete a block still used by the release callback.
            std::atomic<std::uint32_t> weak{1};
            void (*release)(void*) noexcept = nullptr;
        };
    } // namespace detail

    /// THE NON-HOLDING OBSERVER OF A SHARED HANDLE: defined after `shared_object` so its `lock()` can
    /// answer the holding handle directly; this forward declaration lets `shared_object` keep the familiar
    /// `shared_object<T>::weak` spelling as an alias.
    template <typename rhi_object>
    class weak_object;

    /// SHARED HANDLE TO ONE RHI REFERENCE - the contract's counterpart of a shared pointer.
    ///
    /// Copies share ONE control block, so the factory's single reference is released EXACTLY ONCE when the
    /// last copy goes (`release()`, as in `unique_object`). The aliasing constructor keeps a handle to a
    /// DERIVED object while sharing the control block of the object that holds the reference.
    template <typename rhi_object>
    class shared_object {
    public:
        /// the non-holding observer of a shared handle (see `lock()`)
        using weak = weak_object<rhi_object>;

        shared_object() noexcept = default;
        shared_object(std::nullptr_t) noexcept {
        }

        /// take retention of the one reference @p instance carries
        explicit shared_object(rhi_object* const instance) noexcept
            : instance{instance} {
            static_assert(object_type<rhi_object>, "shared_object manages only a contract object handle");
            if (instance != nullptr) {
                this->control = new detail::shared_object_control{&shared_object::release_reference, instance};
            }
        }

        shared_object(unique_object<rhi_object>&& parent) noexcept
            : shared_object(parent.release()) {
        }

        /// a handle to a DERIVED object as a handle to the interface: the SAME control block is shared,
        /// so the one reference is still released exactly once.
        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        shared_object(shared_object<derived_object> const& other) noexcept
            : instance{static_cast<rhi_object*>(other.get())}
            , control{other.control}
            , alias_lifetime{other.alias_lifetime} {
            this->acquire();
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        shared_object(shared_object<derived_object>&& other) noexcept
            : instance{static_cast<rhi_object*>(other.get())}
            , control{other.control}
            , alias_lifetime{std::move(other.alias_lifetime)} {
            other.instance = nullptr;
            other.control = nullptr;
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        shared_object(unique_object<derived_object>&& parent) noexcept
            : shared_object(static_cast<rhi_object*>(parent.release())) {
        }

        /// AN ALIAS TO A NON-HOLDING RHI POINTER tied to an engine holder's lifetime (the engine lease shape):
        /// the holder keeps the resource alive and this handle holds no separate RHI reference.
        /// an ENGINE-HOLDER handle (a test's std::make_shared<fake_root>, say) adopted as an alias: the
        /// shared pointer's reference is the lifetime, so no RHI release runs.
        template <typename parent_type>
            requires std::derived_from<parent_type, rhi_object>
        shared_object(std::shared_ptr<parent_type> parent) noexcept
            : instance{parent.get()}
            , alias_lifetime{std::move(parent)} {
        }

        template <typename parent_type>
        shared_object(std::shared_ptr<parent_type> parent, rhi_object* const pointer) noexcept
            : instance{pointer}
            , alias_lifetime{std::move(parent)} {
        }

        /// the same alias with a CONTRACT-HOLDER root: shares that handle's control block (so the root
        /// outlives the referenced pointer) without releasing A separate extra reference.
        template <typename parent_object>
            requires std::derived_from<parent_object, object>
        shared_object(shared_object<parent_object> const& parent, rhi_object* const pointer) noexcept
            : instance{pointer}
            , control{parent.control} {
            this->acquire();
        }

        shared_object(shared_object const& other) noexcept
            : instance{other.instance}
            , control{other.control}
            , alias_lifetime{other.alias_lifetime} {
            this->acquire();
        }

        shared_object(shared_object&& other) noexcept
            : instance{other.instance}
            , control{other.control}
            , alias_lifetime{std::move(other.alias_lifetime)} {
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
                this->alias_lifetime = std::move(other.alias_lifetime);
                other.instance = nullptr;
                other.control = nullptr;
            }
            return *this;
        }

        ~shared_object() noexcept {
            this->reset();
        }

        /// release this copy's share (the last copy performs the one `release()`)
        void reset() noexcept {
            if (this->control == nullptr) {
                this->instance = nullptr;
                this->alias_lifetime.reset();
                return;
            }
            if (this->control->strong.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                if (this->control->release != nullptr) {
                    this->control->release(this->control->instance);
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
            this->alias_lifetime.swap(other.alias_lifetime);
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

        /// how many shared copies hold the reference (the aliased holder's count for an alias); 0 = empty
        [[nodiscard]] std::uint32_t use_count() const noexcept {
            if (this->control != nullptr) {
                return this->control->strong.load(std::memory_order_acquire);
            }
            return this->alias_lifetime != nullptr ? static_cast<std::uint32_t>(this->alias_lifetime.use_count()) : 0u;
        }
        /// True for a holding reference or lifetime alias, false for an empty handle.
        /// An alias holder must actually retain its target; this cannot inspect arbitrary holder types.
        [[nodiscard]] bool retains_lifetime() const noexcept {
            return instance != nullptr && ((control != nullptr && control->release != nullptr) || alias_lifetime != nullptr);
        }
        [[nodiscard]] bool unique() const noexcept {
            return this->use_count() == 1u;
        }

        /// whether @p other shares THIS handle's control block (the std::shared_ptr::owner_before identity)
        [[nodiscard]] bool shares_control(shared_object const& other) const noexcept {
            return this->control != nullptr ? this->control == other.control
                                            : this->alias_lifetime.get() == other.alias_lifetime.get();
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
        template <typename>
        friend class weak_object;

        void acquire() noexcept {
            if (this->control != nullptr) {
                this->control->strong.fetch_add(1, std::memory_order_relaxed);
            }
        }

        static void release_reference(void* const instance) noexcept {
            static_cast<rhi_object*>(instance)->release();
        }

        rhi_object* instance = nullptr;
        detail::shared_object_control* control = nullptr;
        std::shared_ptr<void const> alias_lifetime;
    };

    /// THE NON-HOLDING OBSERVER OF A SHARED HANDLE, modeled on `std::weak_ptr`: it observes a control block
    /// (or an engine-held alias) without keeping the target alive, and `lock()` answers a `shared_object`
    /// only while that lifetime is still there. `shared_object<T>::weak` is an alias for this spelling, so
    /// existing code keeps working while the observer itself has one top-level definition.
    template <typename rhi_object>
    class weak_object {
    public:
        weak_object() noexcept = default;

        weak_object(weak_object const& other) noexcept
            : instance{other.instance}
            , control{other.control}
            , alias_lifetime{other.alias_lifetime} {
            this->acquire_weak();
        }

        weak_object(weak_object&& other) noexcept
            : instance{other.instance}
            , control{other.control}
            , alias_lifetime{std::move(other.alias_lifetime)} {
            other.instance = nullptr;
            other.control = nullptr;
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        weak_object(shared_object<derived_object> const& parent) noexcept
            : instance{static_cast<rhi_object*>(parent.get())}
            , control{parent.control}
            , alias_lifetime{parent.alias_lifetime} {
            this->acquire_weak();
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        weak_object(weak_object<derived_object> const& other) noexcept
            : instance{static_cast<rhi_object*>(other.instance)}
            , control{other.control}
            , alias_lifetime{other.alias_lifetime} {
            this->acquire_weak();
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        weak_object& operator=(shared_object<derived_object> const& parent) noexcept {
            weak_object copy{parent};
            this->swap(copy);
            return *this;
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        weak_object& operator=(weak_object<derived_object> const& other) noexcept {
            weak_object copy{other};
            this->swap(copy);
            return *this;
        }

        weak_object& operator=(weak_object const& other) noexcept {
            if (this != &other) {
                weak_object copy{other};
                this->swap(copy);
            }
            return *this;
        }

        weak_object& operator=(weak_object&& other) noexcept {
            if (this != &other) {
                this->release_control();
                this->instance = other.instance;
                this->control = other.control;
                this->alias_lifetime = std::move(other.alias_lifetime);
                other.instance = nullptr;
                other.control = nullptr;
            }
            return *this;
        }

        ~weak_object() noexcept {
            this->release_control();
        }

        /// a shared handle while at least one strong reference (or the engine holder) is alive, empty otherwise
        [[nodiscard]] shared_object<rhi_object> lock() const noexcept {
            std::shared_ptr<void const> retained_alias;
            if (this->has_alias()) {
                retained_alias = this->alias_lifetime.lock();
                if (retained_alias == nullptr) {
                    return {};
                }
            }
            if (this->control != nullptr) {
                std::uint32_t count = this->control->strong.load(std::memory_order_acquire);
                while (count != 0) {
                    if (this->control->strong.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                        shared_object<rhi_object> locked;
                        locked.instance = this->instance;
                        locked.control = this->control;
                        locked.alias_lifetime = std::move(retained_alias);
                        return locked;
                    }
                }
                return {};
            }
            if (retained_alias != nullptr) {
                shared_object<rhi_object> locked;
                locked.instance = this->instance;
                locked.alias_lifetime = std::move(retained_alias);
                return locked;
            }
            return {};
        }

        [[nodiscard]] bool expired() const noexcept {
            if (this->control != nullptr) {
                return this->control->strong.load(std::memory_order_acquire) == 0;
            }
            return !this->has_alias() || this->alias_lifetime.expired();
        }

        [[nodiscard]] std::uint32_t use_count() const noexcept {
            if (this->control != nullptr) {
                return this->control->strong.load(std::memory_order_acquire);
            }
            return this->has_alias() ? static_cast<std::uint32_t>(this->alias_lifetime.use_count()) : 0u;
        }

        void reset() noexcept {
            this->release_control();
        }

        void swap(weak_object& other) noexcept {
            rhi_object* const other_instance = other.instance;
            other.instance = this->instance;
            this->instance = other_instance;
            detail::shared_object_control* const other_control = other.control;
            other.control = this->control;
            this->control = other_control;
            this->alias_lifetime.swap(other.alias_lifetime);
        }

        /// whether @p other observes the same control block (or the same engine holder)
        [[nodiscard]] bool shares_control(weak_object const& other) const noexcept {
            if (this->control != nullptr || other.control != nullptr) {
                return this->control == other.control;
            }
            bool const this_has_alias = this->has_alias();
            bool const other_has_alias = other.has_alias();
            if (!this_has_alias || !other_has_alias) {
                return !this_has_alias && !other_has_alias;
            }
            return !this->alias_lifetime.owner_before(other.alias_lifetime) && !other.alias_lifetime.owner_before(this->alias_lifetime);
        }

        /// whether @p other shares this observer's control block or engine-holder identity
        [[nodiscard]] bool shares_control(shared_object<rhi_object> const& other) const noexcept {
            if (this->control != nullptr || other.control != nullptr) {
                return this->control == other.control;
            }
            bool const this_has_alias = this->has_alias();
            bool const other_has_alias = other.alias_lifetime != nullptr;
            if (!this_has_alias || !other_has_alias) {
                return !this_has_alias && !other_has_alias;
            }
            return !this->alias_lifetime.owner_before(other.alias_lifetime) && !other.alias_lifetime.owner_before(this->alias_lifetime);
        }

    private:
        void acquire_weak() noexcept {
            if (this->control != nullptr) {
                this->control->weak.fetch_add(1, std::memory_order_relaxed);
            }
        }

        [[nodiscard]] bool has_alias() const noexcept {
            std::weak_ptr<void const> const empty;
            return this->alias_lifetime.owner_before(empty) || empty.owner_before(this->alias_lifetime);
        }

        void release_control() noexcept {
            if (this->control != nullptr) {
                if (this->control->weak.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    delete this->control;
                }
                this->control = nullptr;
            }
            this->instance = nullptr;
            this->alias_lifetime.reset();
        }

        rhi_object* instance = nullptr;
        detail::shared_object_control* control = nullptr;
        std::weak_ptr<void const> alias_lifetime;
    };

    /// THE CONCRETE-IDENTITY MECHANISM, SHARED BY EVERY BACKEND: a handle's `get_tag()` is the concrete
    /// implementation name (ABI 30), the backend HOLDS the name constants (and exposes the same value as a
    /// static `tag` for compile-time code), and the validate-then-cast operation lives HERE so a second
    /// backend cannot reinvent it (a D3D12 backend validates its names with the same two calls).
    ///
    /// WHY THE GRANULARITY IS CONCRETE, NOT THE INTERFACE: one interface has several concrete
    /// implementations with different layouts behind it (`buffer` is held and, separately, the
    /// backend's referenced read-back slot; `image` is held and the referenced swapchain image), so the
    /// `static_cast` a true answer permits is defined only for the ONE layout the name names.
    ///
    /// A FALSE ANSWER IS A CALLER CONDITION at the function's entrance: a handle this backend did not
    /// hand out, which the contract answers `rhi::error::invalid_argument` (or `nullptr` from a
    /// factory). It is never a panic. The panic belongs to `do_concrete()`, which runs only after the
    /// provenance has already been established - see its note.

    /// @brief whether @p value is the concrete implementation @p name names
    [[nodiscard]] bool is_concrete(object const& value, std::string_view const name) noexcept {
        return value.get_tag() == name;
    }

    /// @brief the concrete implementation behind @p value, as its type
    ///
    /// PANICS ON A MISMATCH, AND THAT IS DELIBERATE: by the time this is called the provenance has
    /// already been established (registry membership, an object-identity comparison, ...), so a name
    /// that does not match is the BACKEND'S bookkeeping being inconsistent - a bug in the code
    /// holding the invariant, which is what `deren::utility::panic` exists for ("a failure the code
    /// EXPECTS travels as a value; a violated invariant terminates"). Callers that instead have a
    /// handle which may legitimately be foreign use `is_concrete()` and answer
    /// `rhi::error::invalid_argument`; they never route a container's caller bug through this cast.
    ///
    /// @param value the handle to cast; MUST already be proven to belong to the calling backend
    /// @param name the concrete name the cast requires, taken from that backend's constants
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
                                      value.get_tag(), name),
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
                                      value.get_tag(), name),
                                  where);
        }
        return static_cast<concrete_t*>(&value);
    }

} // namespace deren::promise::rhi
