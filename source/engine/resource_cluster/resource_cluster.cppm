// module version: 0.1.0
module;
#include <algorithm>
#include <expected>
#include <memory>
#include <string_view>
#include <utility>
export module deren.engine.resource_cluster;
export import deren.utility.shared;
import deren.promise.rhi;

export namespace deren::engine {
    namespace resource_cluster_detail {
        template <typename T>
        struct owner {
            std::shared_ptr<deren::promise::rhi::api_core> device;
            std::shared_ptr<void const> dependency;
            deren::promise::rhi::object_ptr<T> object; // Release before dependency and device.
            owner(std::shared_ptr<deren::promise::rhi::api_core> core, T* resource, std::shared_ptr<void const> parent)
                : device(std::move(core))
                , dependency(std::move(parent))
                , object(resource) {
            }
        };
    } // namespace resource_cluster_detail
    class resource_cluster;

    /// An owning engine lease. get/operator-> borrow the underlying RHI object.
    /// Copies share one factory reference; its release() is called exactly once.
    template <typename T>
    class resource_lease {
        friend class resource_cluster;
        std::shared_ptr<resource_cluster_detail::owner<T>> value;
        T* resource = nullptr;
        explicit resource_lease(std::shared_ptr<resource_cluster_detail::owner<T>> owner) noexcept
            : value(std::move(owner)) {
            resource = value ? value->object.get() : nullptr;
        }

    public:
        resource_lease() noexcept = default;
        [[nodiscard]] T* get() const noexcept {
            return value ? resource : nullptr;
        }
        /// Retain an existing shared owner (including an aliasing generation owner).
        /// The caller supplies the owning core; consumers still validate resource provenance.
        [[nodiscard]] static resource_lease from_shared(
            std::shared_ptr<deren::promise::rhi::api_core> core, std::shared_ptr<T> owned) {
            if (!core || !owned)
                return {};
            resource_lease result;
            result.resource = owned.get();
            result.value = std::allocate_shared<resource_cluster_detail::owner<T>>(
                deren::utility::memory::allocator<resource_cluster_detail::owner<T>>{}, std::move(core), nullptr, std::move(owned));
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

    /// Append-only, single-writer resource ownership. No frame waits or backend API additions.
    /// Escaped leases can outlive the cluster. GPU users must retain a lease/cluster until completion.
    class resource_cluster final {
        struct construction_key {
            explicit construction_key() = default;
        };
        std::shared_ptr<deren::promise::rhi::api_core> core;
        deren::utility::vessel::string label;
        deren::utility::vessel::vector<std::shared_ptr<void const>> members;

        template <typename T>
        std::expected<resource_lease<T>, deren::promise::rhi::error> keep(T* raw, std::shared_ptr<void const> parent = {}) {
            if (!raw)
                return std::unexpected(deren::promise::rhi::error::operation_failed);
            auto owned = std::allocate_shared<resource_cluster_detail::owner<T>>(
                deren::utility::memory::allocator<resource_cluster_detail::owner<T>>{}, core, raw, std::move(parent));
            members.push_back(owned);
            return resource_lease<T>{std::move(owned)};
        }

    public:
        // Pass-key constructor is public only for allocate_shared; creation checks the device first.
        resource_cluster(construction_key, std::shared_ptr<deren::promise::rhi::api_core> device, std::string_view name)
            : core(std::move(device))
            , label(name) {
        }
        resource_cluster(resource_cluster const&) = delete;
        resource_cluster& operator=(resource_cluster const&) = delete;
        resource_cluster(resource_cluster&&) = delete;
        resource_cluster& operator=(resource_cluster&&) = delete;
        ~resource_cluster() {
            while (!members.empty())
                members.pop_back();
        }
        [[nodiscard]] static std::shared_ptr<resource_cluster> create(
            std::shared_ptr<deren::promise::rhi::api_core> device, std::string_view name = {}) {
            if (!device)
                return {};
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
            return keep(core->create_buffer(desc).release());
        }
        [[nodiscard]] auto create_image(deren::promise::rhi::image_desc const& desc) {
            return keep(core->create_image(desc).release());
        }
        [[nodiscard]] auto create_sampler(deren::promise::rhi::sampler_desc const& desc) {
            return keep(core->create_sampler(desc).release());
        }
        [[nodiscard]] std::expected<resource_lease<deren::promise::rhi::image_view>, deren::promise::rhi::error>
        create_image_view(resource_lease<deren::promise::rhi::image> const& image, deren::promise::rhi::image_view_desc const& desc) {
            if (!image || image.value->device.get() != core.get())
                return std::unexpected(deren::promise::rhi::error::invalid_argument);
            return keep(image->make_view(desc).release(), image.value);
        }
        template <typename T>
        [[nodiscard]] deren::promise::rhi::error share(resource_lease<T> const& resource) {
            if (!resource || resource.value->device.get() != core.get())
                return deren::promise::rhi::error::invalid_argument;
            auto const found = std::find_if(members.begin(), members.end(), [&](auto const& entry) { return entry.get() == resource.value.get(); });
            if (found == members.end())
                members.push_back(resource.value);
            return deren::promise::rhi::error::ok;
        }
    };
} // namespace deren::engine
