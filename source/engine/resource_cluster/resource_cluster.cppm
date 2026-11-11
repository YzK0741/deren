// module version: 0.1.1
module;
export module deren.engine.resource_cluster;
export import deren.vstd;
export import deren.utility.shared;
import deren.promise.rhi;

export namespace deren::engine {
    namespace resource_cluster_detail {
        template <typename T>
        struct parent {
            deren::promise::rhi::shared_object<deren::promise::rhi::api_core> device;
            std::shared_ptr<void const> dependency;
            deren::promise::rhi::unique_object<T> object; // Release before dependency and device.
            deren::promise::rhi::shared_object<T> shared; // The shared handle this access exposes.
            parent(deren::promise::rhi::shared_object<deren::promise::rhi::api_core> core, T* resource, std::shared_ptr<void const> parent)
                : device(std::move(core))
                , dependency(std::move(parent))
                , object(resource) {
            }
            parent(deren::promise::rhi::shared_object<deren::promise::rhi::api_core> core, deren::promise::rhi::shared_object<T> handle)
                : device(std::move(core))
                , shared(std::move(handle)) {
            }
        };
    } // namespace resource_cluster_detail

    /// An engine handle over an RHI object: get/operator-> return the underlying object.
    /// Every copy shares the same release(), which runs exactly once, after the last copy is gone.
    template <typename T>
    class resource_handle {
        std::shared_ptr<resource_cluster_detail::parent<T>> value;
        T* resource = nullptr;

    public:
        resource_handle() noexcept = default;
        /// Internal: builds a handle over an existing cluster entry. Callers normally use resource_cluster.
        explicit resource_handle(std::shared_ptr<resource_cluster_detail::parent<T>> parent) noexcept
            : value(std::move(parent)) {
            resource = value ? value->object.get() : nullptr;
        }
        [[nodiscard]] std::shared_ptr<resource_cluster_detail::parent<T>> const& storage() const noexcept {
            return value;
        }
        [[nodiscard]] T* get() const noexcept {
            return value ? resource : nullptr;
        }
        /// Take an existing shared RHI object (including an aliasing generation handle).
        /// The caller supplies the core; consumers still validate resource provenance.
        [[nodiscard]] static resource_handle from_shared(
            deren::promise::rhi::shared_object<deren::promise::rhi::api_core> core, deren::promise::rhi::shared_object<T> resource) {
            if (!core || !resource || !resource.is_live()) {
                return {};
            }
            resource_handle result;
            result.resource = resource.get();
            result.value = std::allocate_shared<resource_cluster_detail::parent<T>>(
                deren::utility::memory::allocator<resource_cluster_detail::parent<T>>{}, std::move(core), std::move(resource));
            return result;
        }
        [[nodiscard]] deren::promise::rhi::api_core* device() const noexcept {
            return value ? value->device.get() : nullptr;
        }
        [[nodiscard]] T* operator->() const noexcept {
            return get();
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return get() != nullptr;
        }
    };

    /// Append-only, single-writer registry of resources the cluster keeps usable. No frame waits or backend API additions.
    /// Escaped accesses can outlive the cluster; every GPU user must keep its access, or the cluster, in scope until its work completes.
    class resource_cluster final {
        struct construction_key {
            explicit construction_key() = default;
        };
        deren::promise::rhi::shared_object<deren::promise::rhi::api_core> core;
        deren::utility::vessel::string label;
        deren::utility::vessel::vector<std::shared_ptr<void const>> members;

        template <typename T>
        std::expected<resource_handle<T>, deren::promise::rhi::error> keep(T* raw, std::shared_ptr<void const> parent = {}) {
            if (!raw) {
                return std::unexpected(deren::promise::rhi::error::operation_failed);
            }
            auto resource = std::allocate_shared<resource_cluster_detail::parent<T>>(
                deren::utility::memory::allocator<resource_cluster_detail::parent<T>>{}, core, raw, std::move(parent));
            members.push_back(resource);
            return resource_handle<T>{std::move(resource)};
        }

    public:
        // Pass-key constructor is public only for allocate_shared; creation checks the device first.
        resource_cluster(construction_key, deren::promise::rhi::shared_object<deren::promise::rhi::api_core> device, std::string_view name)
            : core(std::move(device))
            , label(name) {
        }
        resource_cluster(resource_cluster const&) = delete ("resource_cluster is non-copyable: construct it in place and keep its identity");
        resource_cluster& operator=(resource_cluster const&) = delete ("resource_cluster is non-copyable: configure it in place");
        resource_cluster(resource_cluster&&) = delete ("resource_cluster is non-movable: its resources are tied to the current address");
        resource_cluster& operator=(resource_cluster&&) = delete ("resource_cluster is non-movable: its resources are tied to the current address");
        ~resource_cluster() {
            while (!members.empty()) {
                members.pop_back();
            }
        }
        [[nodiscard]] static std::shared_ptr<resource_cluster> create(
            deren::promise::rhi::shared_object<deren::promise::rhi::api_core> device, std::string_view name = {}) {
            if (!device) {
                return {};
            }
            return std::allocate_shared<resource_cluster>(deren::utility::memory::allocator<resource_cluster>{},
                                                          construction_key{}, std::move(device), name);
        }
        [[nodiscard]] std::string_view name() const noexcept {
            return label;
        }
        [[nodiscard]] deren::promise::rhi::api_core const* device() const noexcept {
            return core.get();
        }
        [[nodiscard]] std::size_t resource_count() const noexcept {
            return members.size();
        }
        [[nodiscard]] bool empty() const noexcept {
            return members.empty();
        }

        [[nodiscard]] auto create_buffer(deren::promise::rhi::buffer_desc const& desc) {
            return keep(core->resources->create_buffer(desc).release());
        }
        [[nodiscard]] auto create_image(deren::promise::rhi::image_desc const& desc) {
            return keep(core->resources->create_image(desc).release());
        }
        [[nodiscard]] auto create_sampler(deren::promise::rhi::sampler_desc const& desc) {
            return keep(core->resources->create_sampler(desc).release());
        }
        [[nodiscard]] std::expected<resource_handle<deren::promise::rhi::image_view>, deren::promise::rhi::error>
        create_image_view(resource_handle<deren::promise::rhi::image> const& image, deren::promise::rhi::image_view_desc const& desc) {
            if (!image || image.storage()->device.get() != core.get()) {
                return std::unexpected(deren::promise::rhi::error::invalid_argument);
            }
            return keep(image->make_view(desc).release(), image.storage());
        }
        template <typename T>
        [[nodiscard]] deren::promise::rhi::error share(resource_handle<T> const& resource) {
            if (!resource || resource.storage()->device.get() != core.get()) {
                return deren::promise::rhi::error::invalid_argument;
            }
            auto const found = std::find_if(members.begin(), members.end(), [&](auto const& entry) { return entry.get() == resource.storage().get(); });
            if (found == members.end()) {
                members.push_back(resource.storage());
            }
            return deren::promise::rhi::error::ok;
        }
    };
} // namespace deren::engine
