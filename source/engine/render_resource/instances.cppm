module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>
export module deren.engine.render_resource.instances;
export import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.render_resource;

export namespace deren::engine::render_resource::instances {
    namespace rhi = deren::promise::rhi;
    enum class lifetime { device,
                          scene,
                          output_size,
                          frame_slot,
                          frame_scratch,
                          history };
    enum class kind { image,
                      buffer,
                      acceleration_structure,
                      micromap,
                      sampler };
    struct micromap_upload {
        deren::utility::vessel::vector<std::byte> data;
        deren::utility::vessel::vector<rhi::micromap_triangle> triangles;
        deren::utility::vessel::vector<uint32_t> indices;
    };
    struct resource_context {
        uint32_t frame_slots = rhi::max_frames_in_flight;
        uint32_t output_images = 1;
        uint32_t shadow_map_size = 2048;
        uint32_t shadow_layers = 1;
        rhi::image_extent render_extent{.width = 1, .height = 1, .depth = 1};
    };
    struct request {
        resource_id id = resource_id::none;
        bool create = true; // false is a dependency on another pass's producer
        lifetime life = lifetime::scene;
        resource_scope scope = resource_scope::device_wide;
        kind type = kind::image;
        rhi::image_desc image{};
        bool layer_views = true;
        rhi::sampler_desc sampler{};
        rhi::buffer_desc buffer{};
        rhi::acceleration_structure_desc structure{};
        rhi::micromap_desc micromap{};
        std::shared_ptr<micromap_upload const> micromap_data;
        deren::utility::vessel::vector<rhi::acceleration_structure_geometry> geometries;
        deren::utility::vessel::vector<std::shared_ptr<void const>> dependencies;
        // Owned immutable bytes are borrowed only during synchronous creation. A small, unique
        // content key stays in generations after the upload bytes have been released by the producer.
        std::shared_ptr<std::byte const> initial_data;
        size_t initial_data_size = 0;
        std::shared_ptr<void const> initialization_key;
        bool zero_on_create = false; // mapped buffer only; initialize each new generation once
        bool zero_on_frame = false;  // mapped frame-slot buffer only; history is preserved
    };
    template <typename T>
    struct borrowed {
        T* value = nullptr;
        T* get() const noexcept {
            return value;
        }
        T* operator->() const noexcept {
            return value;
        }
        T& operator*() const noexcept {
            return *value;
        }
        explicit operator bool() const noexcept {
            return value != nullptr;
        }
    };
    struct resource_instance {
        rhi::unique_object<rhi::image> image;
        rhi::unique_object<rhi::buffer> buffer;
        rhi::unique_object<rhi::acceleration_structure> structure;
        rhi::unique_object<rhi::micromap> micromap;
        rhi::unique_object<rhi::sampler> sampler;
        rhi::unique_object<rhi::image_view> array_view;
        deren::utility::vessel::vector<rhi::unique_object<rhi::image_view>> layers;
    };
    struct generation {
        rhi::shared_object<rhi::api_core> device; // destroyed after all owned objects
        uint64_t version = 0;
        request description{};
        deren::utility::vessel::vector<resource_instance> objects;
    };
    struct resource_set {
        deren::utility::vessel::map<resource_id, std::shared_ptr<generation const>> entries;
        deren::utility::vessel::map<lifetime, deren::utility::vessel::vector<resource_id>> groups;
    };
    class resource_snapshot {
        std::shared_ptr<resource_set const> state;

    public:
        resource_snapshot() = default;
        explicit resource_snapshot(std::shared_ptr<resource_set const> value)
            : state(std::move(value)) {
        }
        std::shared_ptr<generation const> find(resource_id id) const {
            if (!state) {
                return {};
            }
            auto i = state->entries.find(id);
            return i == state->entries.end() ? nullptr : i->second;
        }
        std::span<resource_id const> group(lifetime life) const noexcept {
            if (!state) {
                return {};
            }
            auto i = state->groups.find(life);
            return i == state->groups.end() ? std::span<resource_id const>{} : std::span<resource_id const>{i->second};
        }
        bool same(resource_snapshot const& other) const noexcept {
            return state == other.state;
        }
        size_t size() const noexcept {
            return state ? state->entries.size() : 0;
        }
    };
    class resource_registry {
        rhi::shared_object<rhi::api_core> device_owner;
        std::shared_ptr<resource_set const> active;
        uint64_t next_version = 1;

    public:
        void drain() const {
            if (device_owner) {
                (void)device_owner->wait_idle();
            }
        }
        resource_snapshot snapshot() const {
            return resource_snapshot{active};
        }
        // Admission inspects a complete, unused candidate before it becomes visible to passes.
        // Rejection preserves the active generations, including the cached-reuse case.
        using admission = std::function<rhi::error(resource_snapshot const&)>;
        rhi::error reconcile(rhi::shared_object<rhi::api_core> const& device, resource_context const& context, std::span<request const> requests, admission const& accept = {}) {
            if (!device || context.frame_slots == 0 || context.output_images == 0) {
                return rhi::error::invalid_argument;
            }
            deren::utility::vessel::map<resource_id, request> desired;
            for (auto const& value : requests) {
                if (value.id == resource_id::none || value.id >= resource_id::count) {
                    return rhi::error::invalid_argument;
                }
                if (!value.create) {
                    continue;
                }
                if (!valid(value)) {
                    return rhi::error::invalid_argument;
                }
                auto [i, inserted] = desired.try_emplace(value.id, value);
                if (!inserted) {
                    if (!compatible(i->second, value)) {
                        return rhi::error::invalid_argument;
                    }
                    i->second.image.array_layers = std::max(i->second.image.array_layers, value.image.array_layers);
                    i->second.buffer.size = std::max(i->second.buffer.size, value.buffer.size);
                    i->second.structure.instance_capacity = std::max(i->second.structure.instance_capacity, value.structure.instance_capacity);
                    if (!i->second.initial_data) {
                        i->second.initial_data = value.initial_data;
                    }
                    if (!i->second.micromap_data) {
                        i->second.micromap_data = value.micromap_data;
                    }
                }
            }
            for (auto const& value : requests) {
                if (!desired.contains(value.id)) {
                    return rhi::error::invalid_argument;
                }
            }
            auto next = std::make_shared<resource_set>();
            for (auto const& [id, value] : desired) {
                uint32_t const count = value.scope == resource_scope::per_frame_slot ? context.frame_slots : value.scope == resource_scope::per_swapchain_image ? context.output_images
                                                                                                                                                                : 1u;
                auto previous = snapshot().find(id);
                if (previous && previous->device == device && previous->objects.size() == count &&
                    compatible(previous->description, value) &&
                    (value.type == kind::image ? previous->description.image.array_layers >= value.image.array_layers : value.type == kind::buffer ? previous->description.buffer.size >= value.buffer.size
                                                                                                                                                   : previous->description.structure.instance_capacity >= value.structure.instance_capacity)) {
                    next->entries.emplace(id, previous);
                } else {
                    auto created = std::make_shared<generation>();
                    created->device = device;
                    created->version = next_version++;
                    created->description = value;
                    created->description.initial_data.reset();
                    created->description.micromap_data.reset();
                    created->description.image.debug_name = nullptr;
                    created->objects.reserve(count);
                    for (uint32_t index = 0; index < count; ++index) {
                        resource_instance object;
                        if (value.type == kind::image) {
                            if (value.initialization_key && !value.initial_data) {
                                return rhi::error::not_ready;
                            }
                            auto desc = value.image;
                            if (value.initial_data) {
                                desc.initial_bytes = {value.initial_data.get(), value.initial_data_size};
                            }
                            object.image = rhi::unique_object<rhi::image>{device->create_image(desc)};
                            if (!object.image) {
                                return rhi::error::initialization_failed;
                            }
                            rhi::image_view_desc array{};
                            array.layer_count = 0;
                            array.mip_count = 0;
                            object.array_view = rhi::unique_object<rhi::image_view>{object.image->make_view(array)};
                            if (!object.array_view) {
                                return rhi::error::initialization_failed;
                            }
                            object.layers.reserve(value.layer_views ? value.image.array_layers : 0);
                            for (uint32_t layer = 0; value.layer_views && layer < value.image.array_layers; ++layer) {
                                rhi::image_view_desc desc{};
                                desc.base_layer = layer;
                                desc.layer_count = 1;
                                object.layers.emplace_back(object.image->make_view(desc));
                                if (!object.layers.back()) {
                                    return rhi::error::initialization_failed;
                                }
                            }
                        } else if (value.type == kind::sampler) {
                            object.sampler = rhi::unique_object<rhi::sampler>{device->create_sampler(value.sampler)};
                            if (!object.sampler) {
                                return rhi::error::initialization_failed;
                            }
                        } else if (value.type == kind::micromap) {
                            if (!value.micromap_data) {
                                return rhi::error::not_ready;
                            }
                            auto desc = value.micromap;
                            desc.data = value.micromap_data->data;
                            desc.triangles = value.micromap_data->triangles;
                            desc.indices = value.micromap_data->indices;
                            object.micromap = rhi::unique_object<rhi::micromap>{device->create_micromap(desc)};
                            if (!object.micromap) {
                                return rhi::error::initialization_failed;
                            }
                        } else if (value.type == kind::acceleration_structure) {
                            auto desc = value.structure;
                            desc.geometries = value.geometries.empty() ? nullptr : value.geometries.data();
                            desc.geometry_count = static_cast<uint32_t>(value.geometries.size());
                            object.structure = rhi::unique_object<rhi::acceleration_structure>{device->create_acceleration_structure(desc)};
                            if (!object.structure) {
                                return rhi::error::initialization_failed;
                            }
                        } else {
                            // A released upload can describe an existing generation, but cannot recreate
                            // initialized storage on another device without its source being supplied again.
                            if (value.initialization_key && !value.initial_data) {
                                return rhi::error::not_ready;
                            }
                            auto desc = value.buffer;
                            if (value.initial_data) {
                                desc.initial_bytes = {value.initial_data.get(), value.initial_data_size};
                            }
                            object.buffer = rhi::unique_object<rhi::buffer>{device->create_buffer(desc)};
                            if (!object.buffer) {
                                return rhi::error::initialization_failed;
                            }
                            if ((value.zero_on_frame || value.zero_on_create) && object.buffer->mapped().size() < value.buffer.size) {
                                return rhi::error::unsupported;
                            }
                            if (value.zero_on_create) {
                                auto bytes = object.buffer->mapped();
                                std::fill(bytes.begin(), bytes.end(), std::byte{});
                            }
                        }
                        created->objects.push_back(std::move(object));
                    }
                    next->entries.emplace(id, std::move(created));
                }
                next->groups[value.life].push_back(id);
            }
            if (accept) {
                auto const result = accept(resource_snapshot{next});
                if (result != rhi::error::ok) {
                    return result;
                }
            }
            if (active && device_owner == device && active->entries == next->entries && frame_slots == context.frame_slots) {
                return rhi::error::ok;
            }
            device_owner = device;
            frame_slots = context.frame_slots;
            active = std::move(next);
            return rhi::error::ok;
        }
        // Upload only to a completed slot (or a newly allocated, unused instance). No flush API exists,
        // so cached and GPU-only allocations cannot serve this direct host-write operation.
        rhi::error write_frame_buffer(uint32_t slot, resource_id id, std::span<std::byte const> bytes) {
            if (!active) {
                return rhi::error::invalid_argument;
            }
            auto const entry = active->entries.find(id);
            if (entry == active->entries.end()) {
                return rhi::error::invalid_argument;
            }
            auto const& value = *entry->second;
            if (value.description.type != kind::buffer || value.description.scope != resource_scope::per_frame_slot ||
                slot >= value.objects.size() || bytes.size() > value.description.buffer.size) {
                return rhi::error::invalid_argument;
            }
            auto const usage = value.description.buffer.usage;
            if (usage != rhi::buffer_usage::uniform_coherent && usage != rhi::buffer_usage::storage_coherent &&
                usage != rhi::buffer_usage::readback_coherent) {
                return rhi::error::unsupported;
            }
            auto destination = value.objects[slot].buffer->mapped();
            if (destination.size() < bytes.size()) {
                return rhi::error::unsupported;
            }
            if (!bytes.empty()) {
                std::memmove(destination.data(), bytes.data(), bytes.size());
            }
            return rhi::error::ok;
        }
        // The caller must have completed the GPU wait for this slot before calling this method.
        rhi::error initialize_frame(uint32_t completed_slot, std::span<resource_id const> preserved = {}) {
            if (completed_slot >= frame_slots) {
                return rhi::error::invalid_argument;
            }
            if (!active) {
                return rhi::error::ok;
            }
            for (auto const& [id, value] : active->entries) {
                if (value->description.zero_on_frame && std::find(preserved.begin(), preserved.end(), id) == preserved.end()) {
                    auto bytes = value->objects[completed_slot].buffer->mapped();
                    std::fill(bytes.begin(), bytes.end(), std::byte{});
                }
            }
            return rhi::error::ok;
        }

    private:
        uint32_t frame_slots = rhi::max_frames_in_flight;
        static bool valid(request const& value) {
            if (value.micromap_data && value.type != kind::micromap) {
                return false;
            }
            if (value.initialization_key) {
                if ((value.type != kind::micromap && ((value.type != kind::buffer && value.type != kind::image) || !value.initial_data_size ||
                                                      (value.type == kind::buffer && value.initial_data_size > value.buffer.size))) ||
                    value.zero_on_create || value.zero_on_frame) {
                    return false;
                }
            } else if (value.initial_data || value.initial_data_size) {
                return false;
            }
            if (value.zero_on_create && value.type != kind::buffer) {
                return false;
            }
            switch (value.life) {
            case lifetime::device:
            case lifetime::scene:
            case lifetime::output_size:
            case lifetime::frame_slot:
            case lifetime::frame_scratch:
            case lifetime::history:
                break;
            default:
                return false;
            }
            if (value.scope != resource_scope::device_wide && value.scope != resource_scope::per_frame_slot &&
                value.scope != resource_scope::per_swapchain_image) {
                return false;
            }
            if (value.zero_on_frame && (value.type != kind::buffer || value.scope != resource_scope::per_frame_slot ||
                                        value.life == lifetime::history)) {
                return false;
            }
            if (value.type == kind::image) {
                return value.image.struct_size == sizeof(rhi::image_desc) && value.image.extent.width && value.image.extent.height &&
                       value.image.extent.depth && value.image.array_layers && value.image.format != rhi::image_format::unknown &&
                       value.image.initial_bytes.empty();
            }
            if (value.type == kind::buffer) {
                return value.buffer.struct_size == sizeof(rhi::buffer_desc) && value.buffer.size && value.buffer.initial_bytes.empty();
            }
            if (value.type == kind::sampler) {
                return value.sampler.struct_size == sizeof(rhi::sampler_desc) && std::isfinite(value.sampler.max_lod) && value.sampler.max_lod >= 0;
            }
            if (value.type == kind::micromap) {
                auto const& desc = value.micromap;
                if (!value.initialization_key || value.initial_data || value.initial_data_size ||
                    desc.struct_size != sizeof(rhi::micromap_desc) || !desc.triangle_count || !desc.data_stride ||
                    !desc.data.empty() || !desc.triangles.empty() || !desc.indices.empty() ||
                    (desc.format != rhi::micromap_format::four_state && desc.format != rhi::micromap_format::two_state)) {
                    return false;
                }
                auto const& data = value.micromap_data;
                return !data || (data->data.size() == uint64_t(desc.triangle_count) * desc.data_stride &&
                                 data->triangles.size() == desc.triangle_count && data->indices.size() == desc.triangle_count);
            }
            if (value.type == kind::acceleration_structure) {
                if (value.structure.struct_size != sizeof(rhi::acceleration_structure_desc) ||
                    value.structure.geometries || value.structure.geometry_count) {
                    return false;
                }
                if (value.structure.type == rhi::acceleration_structure_type::top_level) {
                    return value.structure.instance_capacity && value.geometries.empty();
                }
                return value.structure.type == rhi::acceleration_structure_type::bottom_level &&
                       !value.structure.instance_capacity && !value.geometries.empty() && value.geometries.size() <= UINT32_MAX;
            }
            return false;
        }
        static bool compatible(request const& a, request const& b) {
            if (a.dependencies != b.dependencies) {
                return false;
            }
            if (a.initialization_key != b.initialization_key || a.initial_data_size != b.initial_data_size) {
                return false;
            }
            if (a.type != b.type || a.life != b.life || a.scope != b.scope || a.zero_on_frame != b.zero_on_frame || a.zero_on_create != b.zero_on_create) {
                return false;
            }
            if (a.type == kind::buffer) {
                return a.buffer.usage == b.buffer.usage && a.buffer.flags == b.buffer.flags;
            }
            if (a.type == kind::sampler) {
                return a.sampler.address_mode == b.sampler.address_mode && a.sampler.max_lod == b.sampler.max_lod &&
                       a.sampler.mag_filter == b.sampler.mag_filter && a.sampler.min_filter == b.sampler.min_filter &&
                       a.sampler.mipmap_mode == b.sampler.mipmap_mode && a.sampler.compare_enable == b.sampler.compare_enable && a.sampler.compare_op == b.sampler.compare_op;
            }
            if (a.type == kind::micromap) {
                return a.micromap.triangle_count == b.micromap.triangle_count && a.micromap.data_stride == b.micromap.data_stride &&
                       a.micromap.format == b.micromap.format;
            }
            if (a.type == kind::acceleration_structure) {
                if (a.structure.type != b.structure.type || a.structure.flags != b.structure.flags || a.geometries.size() != b.geometries.size()) {
                    return false;
                }
                for (size_t i = 0; i < a.geometries.size(); ++i) {
                    auto const& x = a.geometries[i];
                    auto const& y = b.geometries[i];
                    if (x.vertex_address != y.vertex_address || x.vertex_stride != y.vertex_stride || x.vertex_count != y.vertex_count ||
                        x.index_address != y.index_address || x.index_format != y.index_format || x.index_count != y.index_count || x.opacity_micromap != y.opacity_micromap) {
                        return false;
                    }
                }
                return true;
            }
            return a.image.extent.width == b.image.extent.width && a.image.extent.height == b.image.extent.height &&
                   a.image.extent.depth == b.image.extent.depth && a.image.mip_levels == b.image.mip_levels &&
                   a.image.format == b.image.format && a.image.flags == b.image.flags && a.layer_views == b.layer_views;
        }
    };
} // namespace deren::engine::render_resource::instances
