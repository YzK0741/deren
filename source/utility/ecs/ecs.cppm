module;

export module deren.utility:ecs;
export import deren.vstd;
export import deren.utility.shared;

/**
 * @file ecs.cppm
 * @defgroup ecs Entity Component System
 * @ingroup utility
 * @brief A small sparse-set ECS: entities are generation-checked handles, components live in
 *        per-type pools, and the registry is intentionally single-threaded.
 */
export namespace deren::utility::ecs {
    /**
     * @brief a generation-checked entity handle
     *
     * The index identifies a slot in the registry; the generation invalidates every old handle when
     * that slot is reused. A default-constructed entity is null and is never alive.
     */
    struct entity {
        static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

        std::uint32_t index = invalid_index;
        std::uint32_t generation = 0;

        [[nodiscard]] constexpr bool valid() const noexcept {
            return index != invalid_index;
        }

        friend constexpr bool operator==(entity const&, entity const&) noexcept = default;
    };

    inline constexpr entity null_entity{};

    struct entity_hash {
        [[nodiscard]] std::size_t operator()(entity value) const noexcept {
            std::uint64_t packed =
                (static_cast<std::uint64_t>(value.generation) << 32) | value.index;
            packed ^= packed >> 33;
            packed *= 0xff51afd7ed558ccdULL;
            packed ^= packed >> 33;
            return static_cast<std::size_t>(packed);
        }
    };

    namespace detail {
        [[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view text) noexcept {
            std::uint64_t hash = 14695981039346656037ULL;
            for (char const character : text) {
                hash ^= static_cast<std::uint8_t>(character);
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        template <typename T>
        [[nodiscard]] consteval std::string_view type_name() noexcept {
#if defined(_MSC_VER)
            return __FUNCSIG__;
#else
            return __PRETTY_FUNCTION__;
#endif
        }

        template <typename T>
        [[nodiscard]] consteval std::uint64_t type_hash() noexcept {
            return fnv1a(type_name<T>());
        }

        /**
         * @brief the type-erased half of a component pool
         *
         * Registry-wide operations (`destroy`, `clear`) need to reach every pool without knowing its
         * component type, so each pool exposes exactly those two operations through this interface.
         */
        class component_pool_base {
        public:
            explicit component_pool_base(std::string_view name) noexcept
                : name_(name) {
            }
            virtual ~component_pool_base() = default;

            [[nodiscard]] std::string_view type_name() const noexcept {
                return name_;
            }

            virtual bool erase(std::uint32_t index) noexcept = 0;
            virtual void clear() noexcept = 0;

        private:
            std::string_view name_;
        };

        [[noreturn]] inline void report_type_collision(std::string_view registered,
                                                       std::string_view requested) {
            std::string const message = std::format(
                "ecs: component type id collision between '{}' and '{}'",
                registered,
                requested);
            deren::utility::panic(message);
        }
    } // namespace detail

    /**
     * @brief the type id and type-name used by registry
     *
     * The default id is a consteval FNV-1a hash of the compiler's spelling of the type name. It is
     * stable within one compiler build, but it is not a serialization id and is not stable across
     * compiler families or versions. Specialize this struct with an explicit `id` for components that
     * cross those boundaries.
     */
    template <typename T>
    struct component_traits {
        static constexpr std::uint64_t id = detail::type_hash<T>();
    };

    /**
     * @brief sparse-set storage for one component type
     *
     * `sparse` is indexed by entity index and points into the dense arrays. Erasing uses
     * swap-and-pop, so iteration order is not stable. `T` must be movable because swap-and-pop moves
     * the last value into the erased slot.
     */
    template <typename T>
    class component_pool final : public detail::component_pool_base {
        static_assert(std::is_move_constructible_v<T>);
        static_assert(std::is_move_assignable_v<T>);

    public:
        static constexpr std::uint32_t npos = std::numeric_limits<std::uint32_t>::max();

        component_pool() noexcept
            : detail::component_pool_base(detail::type_name<T>()) {
        }

        [[nodiscard]] bool contains(entity value) const noexcept {
            return try_get(value) != nullptr;
        }

        [[nodiscard]] T* try_get(entity value) noexcept {
            if (value.index >= sparse.size()) {
                return nullptr;
            }
            std::uint32_t const dense = sparse[value.index];
            if (dense == npos || dense >= entities.size() || entities[dense] != value) {
                return nullptr;
            }
            return &values[dense];
        }

        [[nodiscard]] T const* try_get(entity value) const noexcept {
            if (value.index >= sparse.size()) {
                return nullptr;
            }
            std::uint32_t const dense = sparse[value.index];
            if (dense == npos || dense >= entities.size() || entities[dense] != value) {
                return nullptr;
            }
            return &values[dense];
        }

        /**
         * @brief constructs a component only when the entity does not already own one
         * @return the new component, or nullptr for an invalid entity / an existing component
         */
        template <typename... Args>
        T* try_emplace(entity value, Args&&... args) {
            if (!value.valid() || try_get(value) != nullptr) {
                return nullptr;
            }
            return insert(value, std::forward<Args>(args)...);
        }

        /**
         * @brief constructs or replaces the component
         * @return the component, or nullptr for an invalid entity
         */
        template <typename... Args>
        T* emplace_or_replace(entity value, Args&&... args) {
            if (!value.valid()) {
                return nullptr;
            }
            if (T* existing = try_get(value)) {
                std::destroy_at(existing);
                std::construct_at(existing, std::forward<Args>(args)...);
                return existing;
            }
            return insert(value, std::forward<Args>(args)...);
        }

        bool erase(std::uint32_t index) noexcept override {
            if (index >= sparse.size()) {
                return false;
            }
            std::uint32_t const dense = sparse[index];
            if (dense == npos || dense >= entities.size()) {
                return false;
            }

            std::uint32_t const last = static_cast<std::uint32_t>(entities.size() - 1);
            if (dense != last) {
                values[dense] = std::move(values[last]);
                entities[dense] = entities[last];
                sparse[entities[dense].index] = dense;
            }
            values.pop_back();
            entities.pop_back();
            sparse[index] = npos;
            return true;
        }

        void clear() noexcept override {
            values.clear();
            entities.clear();
            sparse.clear();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return values.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return values.empty();
        }

        template <typename F>
        void each(F&& function) {
            for (std::size_t i = 0; i < values.size(); ++i) {
                function(entities[i], values[i]);
            }
        }

        template <typename F>
        void each(F&& function) const {
            for (std::size_t i = 0; i < values.size(); ++i) {
                function(entities[i], values[i]);
            }
        }

    private:
        template <typename... Args>
        T* insert(entity value, Args&&... args) {
            values.emplace_back(std::forward<Args>(args)...);
            entities.push_back(value);
            if (sparse.size() <= value.index) {
                sparse.resize(value.index + 1, npos);
            }
            sparse[value.index] = static_cast<std::uint32_t>(values.size() - 1);
            return &values.back();
        }

        vessel::vector<std::uint32_t> sparse;
        vessel::vector<entity> entities;
        vessel::vector<T> values;
    };

    /**
     * @brief owns entity slots and every component pool
     *
     * Registry operations are not thread-safe. Structural changes invalidate entity handles as
     * described on `destroy`; component references remain valid only until the next operation that
     * moves that pool's storage.
     */
    class registry {
    public:
        registry() = default;
        registry(registry const&) = delete;
        registry& operator=(registry const&) = delete;
        registry(registry&&) noexcept = default;
        registry& operator=(registry&&) noexcept = default;

        [[nodiscard]] entity create() {
            std::uint32_t index = 0;
            if (!free_indices.empty()) {
                index = free_indices.back();
                free_indices.pop_back();
            } else {
                index = static_cast<std::uint32_t>(slots.size());
                slots.emplace_back();
            }

            slot& current = slots[index];
            current.alive = true;
            if (current.generation == 0) {
                current.generation = 1;
            }
            ++live_count;
            return {index, current.generation};
        }

        /**
         * @brief destroys an entity and removes every component it owns
         * @return false when the handle is stale or the entity is already dead
         */
        bool destroy(entity value) noexcept {
            if (!alive(value)) {
                return false;
            }

            for (auto& [id, pool] : pools) {
                (void)id;
                pool->erase(value.index);
            }

            slot& current = slots[value.index];
            current.alive = false;
            ++current.generation;
            if (current.generation == 0) {
                current.generation = 1;
            }
            free_indices.push_back(value.index);
            --live_count;
            return true;
        }

        [[nodiscard]] bool alive(entity value) const noexcept {
            return value.index < slots.size() &&
                   slots[value.index].alive &&
                   slots[value.index].generation == value.generation;
        }

        /**
         * @brief adds a component only when the entity does not already own one
         * @return nullptr for an invalid entity or an existing component
         */
        template <typename T, typename... Args>
        T* try_emplace(entity value, Args&&... args) {
            if (!alive(value)) {
                return nullptr;
            }
            return pool<T>().try_emplace(value, std::forward<Args>(args)...);
        }

        /**
         * @brief adds or replaces a component
         * @return nullptr for an invalid entity
         */
        template <typename T, typename... Args>
        T* emplace_or_replace(entity value, Args&&... args) {
            if (!alive(value)) {
                return nullptr;
            }
            return pool<T>().emplace_or_replace(value, std::forward<Args>(args)...);
        }

        template <typename T>
        [[nodiscard]] T* try_get(entity value) noexcept {
            if (!alive(value)) {
                return nullptr;
            }
            component_pool<T>* found = try_pool<T>();
            return found == nullptr ? nullptr : found->try_get(value);
        }

        template <typename T>
        [[nodiscard]] T const* try_get(entity value) const noexcept {
            if (!alive(value)) {
                return nullptr;
            }
            component_pool<T> const* found = try_pool<T>();
            return found == nullptr ? nullptr : found->try_get(value);
        }

        /**
         * @brief returns a component that the caller must already know exists
         *
         * A missing component is an invariant violation, not a query result. Code that handles
         * arbitrary entities (tools, serializers, scripting bindings) must use `try_get` instead.
         */
        template <typename T>
        [[nodiscard]] T& get(entity value) {
            T* found = try_get<T>(value);
            if (found == nullptr) {
                deren::utility::panic("ecs::get: entity does not have the requested component");
            }
            return *found;
        }

        template <typename T>
        bool remove(entity value) noexcept {
            if (!alive(value)) {
                return false;
            }
            component_pool<T>* found = try_pool<T>();
            return found != nullptr && found->erase(value.index);
        }

        template <typename T>
        [[nodiscard]] bool has(entity value) const noexcept {
            return try_get<T>(value) != nullptr;
        }

        template <typename T, typename F>
        void each(F&& function) {
            if (component_pool<T>* found = try_pool<T>()) {
                found->each(std::forward<F>(function));
            }
        }

        template <typename T, typename F>
        void each(F&& function) const {
            if (component_pool<T> const* found = try_pool<T>()) {
                found->each(std::forward<F>(function));
            }
        }

        template <typename F>
        void each_entity(F&& function) const {
            for (std::uint32_t index = 0; index < slots.size(); ++index) {
                if (slots[index].alive) {
                    function(entity{index, slots[index].generation});
                }
            }
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return live_count;
        }

        void clear() noexcept {
            for (auto& [id, pool] : pools) {
                (void)id;
                pool->clear();
            }
            pools.clear();

            // Keep the slots themselves: their generations are what make old handles stay invalid
            // when an index is reused after clear(). Rebuilding the free list also removes the
            // duplicate entries that a previous destroy()/clear() sequence would otherwise leave.
            free_indices.clear();
            for (std::uint32_t index = 0; index < slots.size(); ++index) {
                slot& current = slots[index];
                if (current.alive) {
                    current.alive = false;
                    ++current.generation;
                    if (current.generation == 0) {
                        current.generation = 1;
                    }
                }
                free_indices.push_back(index);
            }
            live_count = 0;
        }

    private:
        struct slot {
            std::uint32_t generation = 1;
            bool alive = false;
        };

        template <typename T>
        [[nodiscard]] component_pool<T>* try_pool() noexcept {
            auto const found = pools.find(component_traits<T>::id);
            return found == pools.end()
                       ? nullptr
                       : static_cast<component_pool<T>*>(found->second.get());
        }

        template <typename T>
        [[nodiscard]] component_pool<T> const* try_pool() const noexcept {
            auto const found = pools.find(component_traits<T>::id);
            return found == pools.end()
                       ? nullptr
                       : static_cast<component_pool<T> const*>(found->second.get());
        }

        template <typename T>
        component_pool<T>& pool() {
            if (component_pool<T>* found = try_pool<T>()) {
                return *found;
            }

            auto const id = component_traits<T>::id;
            auto const collision = pools.find(id);
            if (collision != pools.end()) {
                detail::report_type_collision(
                    collision->second->type_name(),
                    detail::type_name<T>());
            }

            auto created = std::make_unique<component_pool<T>>();
            component_pool<T>* raw = created.get();
            pools.emplace(id, std::move(created));
            return *raw;
        }

        vessel::vector<slot> slots;
        vessel::vector<std::uint32_t> free_indices;
        vessel::unordered_map<
            std::uint64_t,
            std::unique_ptr<detail::component_pool_base>>
            pools;
        std::size_t live_count = 0;
    };
} // namespace deren::utility::ecs
