// ============================================================================
// THE ACCELERATION-STRUCTURE DEVICE PROBE - the SMALL instrument the S1 migration runs against.
//
// WHY IT EXISTS, AND WHY IT IS SEPARATE FROM THE rt_shadows SMOKE: the only end-to-end ray-tracing
// instrument this repository had is `render-check/rt_smoke.toml` - the full Sponza scene, a 1080x960 frame
// and the whole pass chain. That is the right acceptance for the RENDERER and the wrong instrument for a
// MIGRATION: it needs hundreds of MB, so on a host that is short of memory it fails for reasons that have
// nothing to do with the code under test (measured at HEAD, see the recording-face note), and it reports one
// bit - pass or fail - for a whole frame.
//
// The native-handle exit (its S1 step) moves the acceleration-structure half of the engine behind the
// `ray_tracing` ability. This probe is what says each step of that migration still works, and it is written
// against THE SAME MODULE SURFACE the migration touches - `bottom_level_structures::add()` /
// `record_build()` / `record_update()` and `top_level_structure::begin()` / `add()` / `record_build()` - so
// it keeps its meaning on both sides of the change:
//
//   1. a REAL device, a hidden 64x64 window, VALIDATION ON (the point is the VUIDs the migration could
//      introduce, not a picture);
//   2. a THREE-VERTEX triangle and ONE instance: kilobytes, not scene data;
//   3. the three things the migration is most likely to break, each checked separately:
//        * the CREATE + SIZES path (`add()` twice, one refittable),
//        * the BUILD path (a recorded `vkCmdBuildAccelerationStructuresKHR` that actually executes),
//        * the REFIT path (`record_update()`, which reuses the build's scratch and re-reads the same bytes),
//      plus the module's documented SKIP (a source with no triangles keeps the caller's indices aligned
//      and answers a null handle).
//
// IT SUBMITS THE WORK ITSELF, and that is deliberate: the engine's frame owns the frame's command buffer,
// so an instrument that went through the frame loop would be measuring the frame loop too. The probe creates
// a CALLER-OWNED contract command buffer, records the structures into it, and submits it with a fence through
// the escape - which also makes this file the measured evidence for plan X4's second item (the contract has
// no submit verb for a buffer the CALLER owns; the probes in runtime.probes.cppm do the same raw submit).
//
// THE SAFE DEFAULT: without `--with-device` it prints what it would do and exits 0, so it runs anywhere.
// Run it with the flag in every slice of the migration - that is what it is for.
// ============================================================================
#include "vk_test.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h> // the migrated module's recording takes the driver's command buffer; a TEST may name it (it is not the engine half)

import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.backend_loader;         // load_api_core(): the acquisition lives outside the runtime
import deren.engine.acceleration_structure; // the module S1 migrates (bottom_level_structures / top_level_structure)
import deren.engine.render_resource;
import deren.engine.render_resource.instances;
import deren.engine.ray_tracing;
import deren.engine.pass.mask_bake;
import deren.engine.render_environment;

// After the imports it needs: the header names deren::promise::rhi types (see its own note).
#include "../promise/rhi/backend_entry.hpp"
#include "raster_probe.hpp"
#include "rt_traversal_probe.hpp"

namespace {
    struct managed_structure final : rhi::acceleration_structure {
        int& releases;
        std::uint32_t capacity;
        managed_structure(int& count, std::uint32_t size)
            : acceleration_structure("managed_structure_probe")
            , releases(count)
            , capacity(size) {
        }
        void release() noexcept override {
            ++releases;
            delete this;
        }
        std::uint64_t device_address() const noexcept override {
            return 4096;
        }
        std::uint64_t size_bytes() const noexcept override {
            return 128;
        }
        rhi::error write_instances(std::span<rhi::acceleration_structure_instance const> values) override {
            return values.size() <= capacity ? rhi::error::ok : rhi::error::invalid_argument;
        }
    };
    struct managed_micromap : rhi::micromap {
        managed_micromap()
            : micromap("managed_micromap_probe") {
        }
        void release() noexcept override {
            delete this;
        }
    };
    template <typename Resource>
    void check_micromap_storage(Resource const& value) {
        if constexpr (requires { value.resources->resources().snapshot(); }) {
            auto snapshot = value.resources->resources().snapshot();
            CHECK(snapshot.size() == 1);
            CHECK(snapshot.group(deren::engine::render_resource::instances::lifetime::scene).size() == 1);
        } else {
            CHECK_MSG(false, "Micromaps must be owned by a managed resource subchain");
        }
    }
    struct managed_view : rhi::image_view {
        managed_view()
            : image_view("managed_view") {
        }
        void release() noexcept override {
            delete this;
        }
    };
    struct managed_image : rhi::image {
        rhi::image_desc description;
        explicit managed_image(rhi::image_desc desc)
            : image("managed_image")
            , description(desc) {
            description.initial_bytes = {};
        }
        void release() noexcept override {
            delete this;
        }
        rhi::image_extent extent() const noexcept override {
            return description.extent;
        }
        rhi::image_format format() const noexcept override {
            return description.format;
        }
        rhi::image_view* make_view(rhi::image_view_desc const&) override {
            return new managed_view;
        }
        std::expected<rhi::image_content, rhi::error> get_content(rhi::image_copy_region const&) const override {
            return std::unexpected(rhi::error::unsupported);
        }
    };
    struct managed_sampler : rhi::sampler {
        managed_sampler()
            : sampler("managed_sampler") {
        }
        void release() noexcept override {
            delete this;
        }
    };
    struct managed_core : raster_test::raster_probe_core {
        int image_calls = 0, sampler_calls = 0;
        bool image_fail = false, sampler_fail = false;
        deren::utility::vessel::vector<std::byte> image_upload;
        rhi::image* create_image(rhi::image_desc const& desc) override {
            ++image_calls;
            image_upload.assign(desc.initial_bytes.begin(), desc.initial_bytes.end());
            return image_fail ? nullptr : new managed_image(desc);
        }
        rhi::sampler* create_sampler(rhi::sampler_desc const&) override {
            ++sampler_calls;
            return sampler_fail ? nullptr : new managed_sampler;
        }
        rhi::micromap* create_micromap(rhi::micromap_desc const& desc) override {
            CHECK(desc.triangle_count == desc.triangles.size());
            CHECK(desc.data.size() == desc.triangle_count * desc.data_stride);
            CHECK(desc.indices.size() == desc.triangle_count);
            return new managed_micromap;
        }
        int structure_releases = 0;
        int structure_calls = 0, fail_structure_at = 0;
        rhi::acceleration_structure* create_acceleration_structure(rhi::acceleration_structure_desc const& desc) override {
            if (++structure_calls == fail_structure_at)
                return nullptr;
            return new managed_structure{structure_releases, desc.instance_capacity};
        }
    };
    template <typename Buffer>
    void check_geometry_lease(Buffer& buffer) {
        if constexpr (requires { buffer.lease(); }) {
            auto owner = buffer.lease();
            std::weak_ptr<void const> retained = owner;
            auto* raw = buffer.get();
            CHECK(owner && raw);
            buffer.reset();
            CHECK(!buffer && !retained.expired());
            owner.reset();
            CHECK(retained.expired());
        } else {
            CHECK_MSG(false, "Imported geometry must expose a retained ownership lease");
        }
    }
    template <typename Top>
    void check_top_storage(Top const& top) {
        if constexpr (requires { top.resources(0); }) {
            CHECK(top.resources(0).size() == 2);
            namespace rr = deren::engine::render_resource;
            CHECK(top.resources(0).group(rr::instances::lifetime::frame_slot).size() == 2);
            CHECK(top.resources(0).find(rr::resource_id::top_level_structure)->objects[0].structure);
            CHECK(top.structure(0) == nullptr); // Allocation exists before this frame's build is recorded.
            CHECK(top.resources(0).find(rr::resource_id::rt_instance_table)->objects[0].buffer.get() == top.instance_table_buffer(0));
        } else {
            CHECK_MSG(false, "TLAS and shader instance records must share a managed resource snapshot");
        }
    }
    template <typename Bottom>
    void check_bottom_storage(Bottom const& levels) {
        if constexpr (requires { levels.resources(0); }) {
            CHECK(levels.resources(0).size() == 1);
        } else {
            CHECK_MSG(false, "BLAS creation must be described and owned by a passchain");
        }
    }
    void test_top_growth() {
        namespace as = deren::engine::acceleration_structure;
        managed_core device;
        as::bottom_level_structures levels{device};
        CHECK(levels.add({.vertex_address = 4096, .vertex_stride = 12, .vertex_count = 3}));
        check_bottom_storage(levels);
        as::top_level_structure top{device, 2};
        CHECK(top.begin(0));
        as::instance_source instance;
        for (std::uint32_t i = 0; i < 256; ++i) {
            instance.record.primitive_index = i + 1;
            CHECK(top.add(levels, instance));
        }
        check_top_storage(top);
        auto* before = top.instance_table_buffer(0);
        auto previous = top.resources(0);
        device.fail_structure_at = device.structure_calls + 1;
        instance.record.primitive_index = 257;
        CHECK(!top.add(levels, instance));
        CHECK(top.resources(0).same(previous) && top.instance_table_buffer(0) == before);
        device.fail_structure_at = 0;
        device.fail_at = device.calls + 1;
        instance.record.primitive_index = 257;
        CHECK(!top.add(levels, instance));
        CHECK(top.instance_table_buffer(0) == before && top.instance_table_size(0) == 256 * sizeof(as::instance_record));
        device.fail_at = 0;
        CHECK(top.add(levels, instance));
        auto bytes = const_cast<rhi::buffer*>(top.instance_table_buffer(0))->mapped();
        as::instance_record first{}, last{}, appended{};
        std::memcpy(&first, bytes.data(), sizeof(first));
        std::memcpy(&last, bytes.data() + 255 * sizeof(last), sizeof(last));
        std::memcpy(&appended, bytes.data() + 256 * sizeof(appended), sizeof(appended));
        CHECK(first.primitive_index == 1 && last.primitive_index == 256 && appended.primitive_index == 257);
        CHECK(top.instance_table_buffer(1) == nullptr);
        CHECK(top.begin(0));
        auto const calls = device.calls;
        auto const structure_calls = device.structure_calls;
        CHECK(top.add(levels, instance));
        CHECK(device.calls == calls && device.structure_calls == structure_calls && device.idle_calls == 0);
        raster_test::raster_probe_commands commands;
        CHECK(top.record_build(commands) && top.structure(0));
        auto retained = top.resources(0);
        CHECK(top.begin(0));
        CHECK(top.record_build(commands) && top.structure(0) == nullptr);
        CHECK(top.resources(0).same(retained)); // Empty traversal must not free the reusable allocation.
        CHECK(top.add(levels, instance));
        struct refused_top_commands : raster_test::raster_probe_commands {
            rhi::error build_acceleration_structure(rhi::acceleration_structure&) override {
                return rhi::error::unsupported;
            }
        } refused;
        CHECK(!top.record_build(refused) && top.structure(0) == nullptr);
        CHECK(top.record_build(commands) && top.structure(0));
        CHECK(!top.record_build(refused) && top.structure(0) == nullptr);
        CHECK(top.record_build(commands) && top.structure(0));
    }
    void test_top_lease() {
        namespace as = deren::engine::acceleration_structure;
        auto device = std::make_shared<managed_core>();
        std::weak_ptr<managed_core> root = device;
        auto levels = std::make_unique<as::bottom_level_structures>(*device);
        CHECK(levels->add({.vertex_address = 4096, .vertex_stride = 12, .vertex_count = 3}));
        auto top = std::make_unique<as::top_level_structure>(*device, 2, device);
        CHECK(top->begin(1));
        CHECK(top->add(*levels, {}));
        auto storage = top->resources(1);
        CHECK(storage.size() == 2 && top->resources(0).size() == 0);
        levels.reset(); // The legacy BLAS owner releases while its external core is still alive.
        top.reset();
        device.reset();
        CHECK(!root.expired());
        storage = {};
        CHECK(root.expired());
    }
    void test_bottom_lease() {
        namespace as = deren::engine::acceleration_structure;
        namespace rr = deren::engine::render_resource;
        auto device = std::make_shared<managed_core>();
        std::weak_ptr<managed_core> root = device;
        auto dependency = std::make_shared<int const>(42);
        std::weak_ptr<int const> dependent = dependency;
        deren::utility::vessel::array<std::shared_ptr<void const>, 1> leases{dependency};
        auto levels = std::make_unique<as::bottom_level_structures>(*device, device);
        CHECK(levels->add({}) == 0);
        CHECK(levels->resources(0).size() == 0 && levels->structure(0) == nullptr);
        as::geometry_source source{.vertex_address = 4096, .vertex_stride = 12, .vertex_count = 3};
        CHECK(levels->add(source, true, leases) == 1);
        auto storage = levels->resources(1);
        auto entry = storage.find(rr::resource_id::bottom_level_structure);
        source.vertex_address = 8192;
        CHECK(entry && entry->description.geometries[0].vertex_address == 4096);
        CHECK(entry->description.structure.flags == rhi::to_bits(rhi::acceleration_structure_flag::allow_update));
        for (int i = 0; i < 20; ++i)
            CHECK(levels->add(source));
        CHECK(levels->resources(1).same(storage) && levels->structure(1) == entry->objects[0].structure.get());
        CHECK(device->idle_calls == 0); // Moving entries while growing the vector must not drain them.
        auto count = levels->size();
        device->fail_structure_at = device->structure_calls + 1;
        CHECK(!levels->add(source) && levels->size() == count && levels->resources(1).same(storage));
        entry.reset();
        leases = {};
        dependency.reset();
        levels.reset();
        device.reset();
        CHECK(!root.expired() && !dependent.expired());
        storage = {};
        CHECK(root.expired() && dependent.expired());
    }
    void test_mask_allocation_failure() {
        managed_core device;
        struct probe_caster : deren::engine::primitive {
            void draw(deren::engine::render_environment&) const override {
            }
            void destroy() noexcept override {
            }
            bool is_valid() const noexcept override {
                return true;
            }
        } caster;
        caster.vertex_buffer = rhi::object_manager<rhi::buffer>{device.create_buffer({.size = 192, .usage = rhi::buffer_usage::storage_gpu_only})};
        caster.index_buffer = rhi::object_manager<rhi::buffer>{device.create_buffer({.size = 12, .usage = rhi::buffer_usage::storage_gpu_only})};
        caster.vertex_stride = 32;
        caster.vertex_count = caster.draw_index_count = 3;
        deren::utility::vessel::array<deren::engine::primitive const*, 1> casters{&caster};
        deren::engine::material_record material;
        material.flags = 16;
        int recorded = 0;
        deren::engine::ray_tracing::build_inputs inputs;
        inputs.casters = casters;
        inputs.materials = std::span{&material, 1};
        inputs.mask_bake = true;
        inputs.hooks.owner = &recorded;
        inputs.hooks.mask_ready = [](void*) { return true; };
        inputs.hooks.record_mask_bake = [](void* owner, rhi::command_buffer&, deren::engine::pass::mask_bake_request const&) { ++*static_cast<int*>(owner); };
        raster_test::raster_probe_commands commands;
        device.fail_structure_at = 1;
        deren::engine::ray_tracing::structure_set phase{device};
        CHECK(!phase.build(commands, inputs));
        CHECK(recorded == 0); // No unsubmitted command may refer to the abandoned mask destination.
        device.fail_structure_at = device.structure_calls + 2;
        deren::utility::vessel::array<deren::engine::primitive const*, 2> two{&caster, &caster};
        inputs.casters = two;
        deren::engine::ray_tracing::structure_set later{device};
        CHECK(!later.build(commands, inputs) && recorded == 0);
        device.fail_structure_at = 0;
        deren::engine::ray_tracing::structure_set success{device};
        CHECK(success.build(commands, inputs) && success.ready() && recorded == 2);
        namespace rr = deren::engine::render_resource;
        auto mask = success.bake_resources(false, 0).find(rr::resource_id::rt_mask_vertices);
        CHECK(mask && mask->description.life == rr::instances::lifetime::scene && mask->objects[0].buffer->size() == 96);
        CHECK(success.micromaps().size() == 2);
        check_micromap_storage(success.micromaps()[0]);
        struct refused_micromap_commands : raster_test::raster_probe_commands {
            int builds = 0;
            bool refuse_barrier = false;
            rhi::error build_micromap(rhi::micromap&) override {
                return refuse_barrier ? rhi::error::ok : rhi::error::unsupported;
            }
            rhi::error build_acceleration_structure(rhi::acceleration_structure&) override {
                ++builds;
                return rhi::error::ok;
            }
            rhi::error barrier(rhi::barrier_group const& group) override {
                return refuse_barrier && group.has_memory && group.memory.from == rhi::buffer_use::micromap_write ? rhi::error::unsupported : rhi::error::ok;
            }
        } refused_mm;
        deren::engine::ray_tracing::structure_set failed_mm{device};
        CHECK(!failed_mm.build(refused_mm, inputs) && !failed_mm.ready() && refused_mm.builds == 0);
        CHECK(failed_mm.micromaps().size() == 2);
        refused_mm.refuse_barrier = true;
        refused_mm.builds = 0;
        deren::engine::ray_tracing::structure_set failed_barrier{device};
        CHECK(!failed_barrier.build(refused_mm, inputs) && !failed_barrier.ready() && refused_mm.builds == 0);
        struct refused_commands : raster_test::raster_probe_commands {
            int calls = 0;
            rhi::error build_acceleration_structure(rhi::acceleration_structure&) override {
                return ++calls == 2 ? rhi::error::unsupported : rhi::error::ok;
            }
        } refused;
        auto released = device.structure_releases;
        deren::engine::ray_tracing::structure_set recording_failure{device};
        CHECK(!recording_failure.build(refused, inputs) && !recording_failure.ready());
        CHECK(device.structure_releases == released); // The first recorded build still borrows its AS.
        inputs.mask_bake = false;
        inputs.skin_bake = true;
        caster.vertex_stride = 64;
        caster.push.skin_base = 1;
        inputs.hooks.skin_ready = [](void*) { return true; };
        inputs.hooks.record_skin = [](void*, rhi::command_buffer&, std::span<deren::engine::ray_tracing::caster_level const>) { return true; };
        deren::engine::ray_tracing::structure_set skinned{device};
        CHECK(skinned.build(commands, inputs) && skinned.ready());
        auto skin = skinned.bake_resources(true, 0).find(rr::resource_id::rt_skin_vertices);
        CHECK(skin && skin->description.buffer.usage == rhi::buffer_usage::storage_gpu_only && skin->objects[0].buffer->size() == 96);
        CHECK(skinned.update(commands, 0, inputs) && skinned.handle(0));
        inputs.hooks.record_skin = [](void*, rhi::command_buffer&, std::span<deren::engine::ray_tracing::caster_level const>) { return false; };
        auto const failed_update = skinned.update(commands, 0, inputs);
        CHECK(!failed_update && failed_update.error().disable_skin_bake && skinned.handle(0) == nullptr);
        deren::engine::ray_tracing::structure_set skin_failure{device};
        auto failed_skin = skin_failure.build(commands, inputs);
        CHECK(!failed_skin && failed_skin.error().disable_skin_bake && !skin_failure.ready());
        CHECK(skin_failure.bake_resources(true, 0).size() == 1); // A partial skin dispatch may still borrow it.
    }
    void test_micromap_upload_lifetime() {
        namespace rr = deren::engine::render_resource;
        namespace managed = rr::instances;
        auto device = std::make_shared<managed_core>();
        managed::resource_registry registry;
        auto payload = std::make_shared<managed::micromap_upload>();
        payload->data.assign(4, std::byte{3});
        payload->triangles.push_back({.format = static_cast<uint16_t>(rhi::micromap_format::four_state)});
        payload->indices.push_back(0);
        std::weak_ptr<managed::micromap_upload const> source = payload;
        managed::request request;
        request.id = rr::resource_id::rt_opacity_micromap;
        request.type = managed::kind::micromap;
        request.micromap.triangle_count = 1;
        request.micromap_data = payload;
        request.initialization_key = std::make_shared<int const>(0);
        CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::ok);
        auto original = registry.snapshot();
        auto entry = original.find(request.id);
        CHECK(entry && entry->objects[0].micromap && !entry->description.micromap_data);
        request.micromap_data.reset();
        payload.reset();
        CHECK(source.expired());
        CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::ok);
        CHECK(registry.snapshot().find(request.id) == entry);
        request.initialization_key = std::make_shared<int const>(1);
        CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::not_ready);
        CHECK(registry.snapshot().same(original));
        request.micromap.data = std::span{reinterpret_cast<std::byte const*>("x"), 1};
        CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::invalid_argument);
        request.micromap.data = {};
        request.micromap_data = std::make_shared<managed::micromap_upload>();
        CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::invalid_argument);
        CHECK(registry.snapshot().same(original));
        std::weak_ptr<managed_core> root = device;
        CHECK(registry.reconcile(device, {}, {}) == rhi::error::ok);
        registry = {};
        device.reset();
        CHECK(!root.expired());
        original = {};
        entry.reset();
        CHECK(root.expired());
    }
    void test_managed_geometry_dependency() {
        namespace rr = deren::engine::render_resource;
        namespace as = deren::engine::acceleration_structure;
        auto device = std::make_shared<managed_core>();
        std::weak_ptr<managed_core> root = device;
        rr::instances::resource_registry registry;
        rr::instances::request request;
        request.id = rr::resource_id::scene_geometry_vertices;
        request.type = rr::instances::kind::buffer;
        request.buffer = {.size = 96};
        CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::ok);
        auto generation = registry.snapshot().find(request.id);
        std::weak_ptr<rr::instances::generation const> geometry = generation;
        deren::engine::normal_draw_primitive leaf;
        leaf.vertex_buffer.bind(generation->objects[0].buffer.get(), generation);
        auto levels = std::make_unique<as::bottom_level_structures>(*device, device);
        deren::utility::vessel::array<std::shared_ptr<void const>, 1> leases{leaf.vertex_buffer.lease()};
        CHECK(levels->add({.vertex_address = 4096, .vertex_stride = 32, .vertex_count = 3}, false, leases));
        auto retained = levels->resources(0);
        leaf.destroy();
        leases = {};
        generation.reset();
        registry = {};
        levels.reset();
        device.reset();
        CHECK(!geometry.expired() && !root.expired());
        retained = {};
        CHECK(geometry.expired() && root.expired());
    }
    void test_image_upload_lifetime() {
        namespace rr = deren::engine::render_resource;
        auto device = std::make_shared<managed_core>();
        rr::instances::resource_registry registry;
        rr::instances::request request;
        request.id = rr::resource_id::white_texture;
        request.image = {.extent = {1, 1, 1}, .format = rhi::image_format::rgba8_unorm, .initial_bytes = {}};
        auto data = std::make_shared<deren::utility::vessel::vector<std::byte>>(4, std::byte{0x73});
        std::weak_ptr<deren::utility::vessel::vector<std::byte>> source = data;
        request.initial_data = std::shared_ptr<std::byte const>{data, data->data()};
        request.initial_data_size = 4;
        request.initialization_key = std::make_shared<int const>(0);
        auto const prepared = registry.reconcile(device, {}, std::span{&request, 1});
        CHECK(prepared == rhi::error::ok);
        if (prepared != rhi::error::ok)
            return;
        auto original = registry.snapshot();
        CHECK(device->image_upload == *data && !original.find(request.id)->description.initial_data);
        request.initial_data.reset();
        data.reset();
        CHECK(source.expired());
        CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::ok && device->image_calls == 1);
        request.initialization_key = std::make_shared<int const>(1);
        CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::not_ready);
        CHECK(registry.snapshot().same(original) && device->image_calls == 1);
    }
    template <typename Request>
    void test_sampler_storage() {
        if constexpr (requires(Request value) { value.sampler; }) {
            auto device = std::make_shared<managed_core>();
            deren::engine::render_resource::instances::resource_registry registry;
            Request request;
            request.id = deren::engine::render_resource::resource_id::texture_sampler;
            using resource_kind = decltype(request.type);
            request.type = resource_kind::sampler;
            request.life = deren::engine::render_resource::instances::lifetime::device;
            CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::ok);
            auto original = registry.snapshot();
            CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::ok && device->sampler_calls == 1);
            request.sampler.max_lod = 3;
            device->sampler_fail = true;
            CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::initialization_failed);
            CHECK(registry.snapshot().same(original));
            device->sampler_fail = false;
            CHECK(registry.reconcile(device, {}, std::span{&request, 1}) == rhi::error::ok && !registry.snapshot().same(original));
        } else {
            CHECK_MSG(false, "Shared samplers need typed managed declarations");
        }
    }
} // namespace

namespace {
    namespace rhi = deren::promise::rhi;
    /// the migrated module's own namespace (geometry_source, ottom_level_structures, ...)
    namespace as = deren::engine::acceleration_structure;

    /// Is this run allowed to build a device? The flag is required rather than defaulted: the device path is
    /// slow (a real instance, device and swapchain) and a machine without a Vulkan device must still run the
    /// no-flag half.
    [[nodiscard]] bool wants_device(int const argc, char** const argv) {
        for (int i = 1; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--with-device") {
                return true;
            }
        }
        return false;
    }

    /// A three-vertex triangle at unit scale, and the frame's only geometry.
    constexpr std::uint32_t triangle_vertices = 3u;
    struct vertex {
        float position[3];
    };

    /// The device the probe asks for: 64x64 and HIDDEN. A visible window does not come up in this
    /// environment at all (the note in source/tests/test_runtime_dyn.cpp records the hang), and the picture is not
    /// what this instrument measures.
    [[nodiscard]] rhi::create_info probe_create_info() {
        rhi::create_info creation{};
        creation.window_title = "deren acceleration-structure probe";
        creation.window_width = 64;
        creation.window_height = 64;
        creation.window_visible = false;
        creation.validation_layers = true; // the point of the instrument: VUIDs the migration could introduce
        return creation;
    }
} // namespace

int main(int const argc, char** const argv) {
    test_top_growth();
    test_top_lease();
    test_bottom_lease();
    test_mask_allocation_failure();
    test_micromap_upload_lifetime();
    test_managed_geometry_dependency();
    test_image_upload_lifetime();
    test_sampler_storage<deren::engine::render_resource::instances::request>();
    {
        managed_core device;
        deren::engine::normal_draw_primitive leaf;
        leaf.vertex_buffer = rhi::object_manager<rhi::buffer>{device.create_buffer({.size = 96})};
        check_geometry_lease(leaf.vertex_buffer);
    }
    deren::vk_test::write_line("as_probe: this executable compiled abi {}", rhi::abi_version);
    // ABI34: the native-vocabulary cleanup removed a virtual from vulkan_escape (vtable shift), so the
    // handshake number moved with it.
    CHECK(rhi::abi_version == 34u);

    if (!wants_device(argc, argv)) {
        deren::vk_test::write_line("as_probe: device path skipped (pass --with-device to run the real instrument)");
        return deren::vk_test::finish("test_acceleration_structures");
    }

    rhi::create_info const creation = probe_create_info();
    std::shared_ptr<rhi::api_core> core = deren::engine::load_api_core(creation);
    CHECK(core != nullptr);
    if (core == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }

    // ---- 1. THE DEVICE'S HALF: the escape (native handles, the probe's own submit) and the capability that
    //         decides whether acceleration structures exist at all on this device.
    rhi::vulkan_escape* const escape = rhi::query_extension<rhi::vulkan_escape>(*core);
    CHECK(escape != nullptr);
    rhi::device_capabilities* const capabilities = rhi::query_extension<rhi::device_capabilities>(*core);
    CHECK(capabilities != nullptr); // announced unconditionally by this backend (see the ability's note)
    if (escape == nullptr || capabilities == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }
    deren::vk_test::write_line("as_probe: mesh_shader={} ray_query={} max_push_constants={} graphics_queue_family={}",
                               capabilities->mesh_shader(), capabilities->ray_query(), capabilities->max_push_constants_size(),
                               capabilities->graphics_queue_family());

    // A DEVICE WITHOUT THE EXTENSION IS A SKIP, NOT A FAILURE: the question this probe answers is "does the
    // engine's acceleration-structure path work", and on a device that cannot have one there is nothing to
    // answer - the same "capability first" order every path in the engine follows.
    bool acceleration_structure_available = false;
    for (char const* const enabled : escape->enabled_device_extensions()) {
        if (enabled != nullptr && std::strcmp(enabled, "VK_KHR_acceleration_structure") == 0) {
            acceleration_structure_available = true;
        }
    }
    if (!acceleration_structure_available) {
        deren::vk_test::write_line("as_probe: VK_KHR_acceleration_structure is not enabled on this device - skipping");
        return deren::vk_test::finish("test_acceleration_structures");
    }

    rhi::device_address* const addresses = rhi::query_extension<rhi::device_address>(*core);
    CHECK(addresses != nullptr);
    if (addresses == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }

    // ---- 2. THE GEOMETRY: three vertices and three indices, in buffers the build can READ - which is the
    //         `device_address` + `acceleration_structure_input` pair the renderer's own build inputs carry.
    constexpr rhi::buffer_flags build_input_flags =
        rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::acceleration_structure_input);
    bool const multi_geometry = std::any_of(argv + 1, argv + argc, [](char const* arg) { return std::string_view(arg) == "--multi-geometry"; });
    deren::utility::vessel::vector<vertex> vertices{{.position = {0.0f, 0.0f, 0.0f}},
                                                    {.position = {1.0f, 0.0f, 0.0f}},
                                                    {.position = {0.0f, 1.0f, 0.0f}},
                                                    {.position = {10.0f, 0.0f, 0.0f}},
                                                    {.position = {11.0f, 0.0f, 0.0f}},
                                                    {.position = {10.0f, 1.0f, 0.0f}}};
    std::uint32_t const indices[6] = {0u, 1u, 2u, 0u, 1u, 2u};
    if (!multi_geometry) {
        vertices.resize(triangle_vertices);
    }
    auto const index_data = std::span<std::uint32_t const>(indices).first(multi_geometry ? 6u : triangle_vertices);
    rhi::buffer* const vertex_buffer = core->create_buffer(rhi::buffer_desc{
        .size = vertices.size() * sizeof(vertex), .usage = rhi::buffer_usage::storage_coherent, .flags = build_input_flags});
    rhi::buffer* const index_buffer = core->create_buffer(rhi::buffer_desc{
        .size = index_data.size_bytes(), .usage = rhi::buffer_usage::storage_coherent, .flags = build_input_flags});
    CHECK(vertex_buffer != nullptr);
    CHECK(index_buffer != nullptr);
    if (vertex_buffer == nullptr || index_buffer == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }
    std::memcpy(vertex_buffer->mapped().data(), vertices.data(), vertices.size() * sizeof(vertex));
    std::memcpy(index_buffer->mapped().data(), index_data.data(), index_data.size_bytes());

    as::geometry_source const source{
        .vertex_address = addresses->buffer_address(*vertex_buffer, 0),
        .vertex_stride = sizeof(vertex),
        .vertex_count = triangle_vertices,
        .index_address = addresses->buffer_address(*index_buffer, 0),
        .index_type = deren::promise::rhi::index_type::uint32,
        .index_count = triangle_vertices,
    };
    deren::vk_test::write_line("as_probe: geometry vertex_address={:#x} index_address={:#x} triangles={}",
                               source.vertex_address, source.index_address, source.index_count / 3u);
    CHECK(source.vertex_address != 0u);
    CHECK(source.index_address != 0u);

    // ---- 3. THE COMMAND BUFFER THE PROBE OWNS (see the header): the contract made it, the escape turns it
    //         into the native handle the module's recording takes, and the probe submits it itself.
    rhi::command_buffer* const commands = core->create_command_buffer({});
    CHECK(commands != nullptr);
    if (commands == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }
    CHECK(commands->begin_recording({}) == rhi::error::ok);
    auto* const native_commands = static_cast<VkCommandBuffer>(escape->native_command_buffer(*commands));
    CHECK(native_commands != VK_NULL_HANDLE);

    // ---- 4. THE STRUCTURES, THROUGH THE TIER-1 INTERFACE (abi 26): the two levels, the refit the flag
    //         declares, and the refusals the interface promises - each as its own check.
    //
    //         THE BACKEND OWNS EVERYTHING THE CALLER DOES NOT SEE (the storage, the scratch, the alignment,
    //         the sizes), which is why this section has no size query, no buffer and no offset in it: that is
    //         the whole difference from the engine module's own path, which the probe drove before this batch.
    rhi::acceleration_structure_geometry const geometry{
        .vertex_address = addresses->buffer_address(*vertex_buffer, 0),
        .vertex_stride = sizeof(vertex),
        .vertex_count = triangle_vertices,
        .index_address = addresses->buffer_address(*index_buffer, 0),
        .index_format = rhi::index_type::uint32,
        .index_count = triangle_vertices,
    };
    // --multi-geometry uses different counts, with the probe's hit triangle in the SECOND geometry.
    // Missing per-geometry ranges either read past the first triangle or lose the ray hit.
    auto away_geometry = geometry;
    away_geometry.vertex_address += triangle_vertices * sizeof(vertex);
    away_geometry.index_address = 0;
    away_geometry.index_count = 0;
    auto hit_geometry = geometry;
    hit_geometry.index_count = 6u;
    deren::utility::vessel::array<rhi::acceleration_structure_geometry, 2> const geometries{away_geometry, hit_geometry};
    rhi::acceleration_structure* const blas = core->create_acceleration_structure(rhi::acceleration_structure_desc{
        .type = rhi::acceleration_structure_type::bottom_level,
        .flags = rhi::to_bits(rhi::acceleration_structure_flag::allow_update),
        .geometries = multi_geometry ? geometries.data() : &geometry,
        .geometry_count = multi_geometry ? static_cast<std::uint32_t>(geometries.size()) : 1u,
    });
    CHECK(blas != nullptr);
    rhi::acceleration_structure* const static_blas = core->create_acceleration_structure(rhi::acceleration_structure_desc{
        .type = rhi::acceleration_structure_type::bottom_level,
        .geometries = &geometry,
        .geometry_count = 1u,
    });
    CHECK(static_blas != nullptr);
    rhi::acceleration_structure* const tlas = core->create_acceleration_structure(rhi::acceleration_structure_desc{
        .type = rhi::acceleration_structure_type::top_level,
        .instance_capacity = 1u,
    });
    CHECK(tlas != nullptr);
    if (blas == nullptr || static_blas == nullptr || tlas == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }
    deren::vk_test::write_line("as_probe: created blas address={:#x} size={} bytes, tlas address={:#x} size={} bytes",
                               blas->device_address(), blas->size_bytes(), tlas->device_address(), tlas->size_bytes());
    CHECK(blas->device_address() != 0u); // what a shader and an instance record reach it by
    CHECK(blas->size_bytes() != 0u);     // what an address-range descriptor would carry
    CHECK(tlas->device_address() != 0u);

    // THE INSTANCE the traversal reads: an identity 3x4 transform, the BLAS's own address as its reference.
    rhi::acceleration_structure_instance instance{};
    instance.transform[0] = 1.0f;
    instance.transform[5] = 1.0f;
    instance.transform[10] = 1.0f;
    instance.structure_reference = blas->device_address();
    CHECK(tlas->write_instances(std::span<rhi::acceleration_structure_instance const>(&instance, 1u)) == rhi::error::ok);
    // (a) THE CAPACITY REFUSAL: one past what it was created with - the array behind it is sized for one.
    rhi::acceleration_structure_instance const too_many[] = {instance, instance};
    CHECK(tlas->write_instances(too_many) == rhi::error::invalid_argument);
    // (b) A BOTTOM LEVEL HAS NO INSTANCE LIST, refused by name rather than silently dropped.
    CHECK(blas->write_instances(std::span<rhi::acceleration_structure_instance const>(&instance, 1u)) == rhi::error::unsupported);

    // ---- 5. THE RECORDING: both builds, the refit the flag allows, and the one it does not.
    CHECK(commands->build_acceleration_structure(*blas) == rhi::error::ok);
    CHECK(commands->build_acceleration_structure(*static_blas) == rhi::error::ok);
    CHECK(commands->build_acceleration_structure(*tlas) == rhi::error::ok);
    CHECK(commands->refit_acceleration_structure(*blas) == rhi::error::ok);                 // created with allow_update
    CHECK(commands->refit_acceleration_structure(*static_blas) == rhi::error::unsupported); // ... and this one was not

    // ---- 5a. THE ACCELERATION-STRUCTURE BARRIER EDGE (RT stage 1): the OBJECT-level pair the RT consumers need -
    //          the build that wrote a structure against the shaders that traverse it - plus the two refusals that
    //          keep the vocabulary honest (no resource / a use nobody defined). It is recorded through the same
    //          batch the images and buffers use, which is the point: one dependency info carries all three.
    rhi::acceleration_structure_barrier const traversable{
        .resource = tlas,
        .from = rhi::acceleration_structure_use::build_write,
        .to = rhi::acceleration_structure_use::shader_read,
    };
    CHECK(commands->barrier(rhi::barrier_group{
              .struct_size = sizeof(rhi::barrier_group),
              .acceleration_structures = std::span<rhi::acceleration_structure_barrier const>(&traversable, 1u),
          }) == rhi::error::ok);
    rhi::acceleration_structure_barrier const without_resource{
        .resource = nullptr,
        .from = rhi::acceleration_structure_use::build_write,
        .to = rhi::acceleration_structure_use::shader_read,
    };
    CHECK(commands->barrier(rhi::barrier_group{
              .struct_size = sizeof(rhi::barrier_group),
              .acceleration_structures = std::span<rhi::acceleration_structure_barrier const>(&without_resource, 1u),
          }) == rhi::error::invalid_argument);
    rhi::acceleration_structure_barrier const undefined_use{
        .resource = tlas,
        .from = rhi::acceleration_structure_use::undefined,
        .to = rhi::acceleration_structure_use::shader_read,
    };
    CHECK(commands->barrier(rhi::barrier_group{
              .struct_size = sizeof(rhi::barrier_group),
              .acceleration_structures = std::span<rhi::acceleration_structure_barrier const>(&undefined_use, 1u),
          }) == rhi::error::unsupported);

    // ---- 5b. THE ENGINE MODULE ON TOP OF IT (plan S1's P1b-2): `bottom_level_structures` is a thin front end
    //          over the same interface now, and this is what says so on a real device - it adds the same
    //          geometry, records its build and a refit, and hands back the object the runtime binds.
    // Recorded commands borrow their structures and inputs until submission has completed.
    as::bottom_level_structures levels{*core};
    rhi::object_manager<rhi::micromap> micromap_owner;
    rhi::object_manager<rhi::acceleration_structure> shaded_owner;
    rhi::object_manager<rhi::acceleration_structure> triangle_soup;
    {
        rhi::acceleration_structure_geometry const unindexed{
            .vertex_address = source.vertex_address,
            .vertex_stride = source.vertex_stride,
            .vertex_count = triangle_vertices,
        };
        triangle_soup = rhi::object_manager<rhi::acceleration_structure>{core->create_acceleration_structure(rhi::acceleration_structure_desc{
            .type = rhi::acceleration_structure_type::bottom_level,
            .geometries = &unindexed,
            .geometry_count = 1u,
        })};
        CHECK(triangle_soup);
        if (triangle_soup) {
            CHECK(commands->build_acceleration_structure(*triangle_soup) == rhi::error::ok);
        }
        // (a) THE DOCUMENTED SKIP: a source with no triangles keeps the caller's index alignment.
        auto const skipped = levels.add(as::geometry_source{.vertex_count = 0, .index_count = 0});
        CHECK(skipped.has_value() && *skipped == 0u);
        CHECK(levels.structure(*skipped) == nullptr);
        auto const via_module = levels.add(source, /*refittable=*/true);
        CHECK_MSG(via_module.has_value(), via_module.has_value() ? nullptr : via_module.error().c_str());
        if (via_module.has_value()) {
            CHECK(levels.size() == 2u);
            CHECK(levels.structure(*via_module) != nullptr);
            CHECK(levels.structure(*via_module)->device_address() != 0u);
            auto const built = levels.record_build(*commands);
            CHECK_MSG(built.has_value(), built.has_value() ? nullptr : built.error().c_str());
            std::uint32_t const refit[] = {*via_module};
            auto const refitted = levels.record_update(*commands, refit);
            CHECK_MSG(refitted.has_value(), refitted.has_value() ? nullptr : refitted.error().c_str());
            deren::vk_test::write_line("as_probe: the module built and refitted through the tier-1 interface: {} geometries",
                                       levels.last_stats().geometry_count);
        }
        // (b) THE OPACITY MICROMAP, THROUGH THE SAME INTERFACE (plan S1's P4): one micro-triangle, UNKNOWN, and a
        //     geometry that consults it. THE ORDER IS THE CALLER'S AND IT MATTERS: the micromap is built before
        //     the geometry that reads it, which is what the two record calls below do.
        if (capabilities->mesh_shader() || true) {                                           // the micromap path is independent of the mesh one; kept flat on purpose
            deren::utility::vessel::vector<std::byte> const attributes(4u, std::byte{0x03}); // the 4-state "unknown" pair
            rhi::micromap_triangle const record{
                .data_offset = 0u,
                .subdivision_level = 0u,
                .format = static_cast<std::uint16_t>(rhi::micromap_format::four_state),
            };
            std::uint32_t const micro_index = 0u;
            rhi::micromap* const micromap = core->create_micromap(rhi::micromap_desc{
                .triangle_count = 1u,
                .data = attributes,
                .data_stride = 4u,
                .triangles = std::span<rhi::micromap_triangle const>(&record, 1u),
                .indices = std::span<std::uint32_t const>(&micro_index, 1u),
                .format = rhi::micromap_format::four_state,
            });
            CHECK(micromap != nullptr); // a device without VK_EXT_opacity_micromap answers null with a named log
            if (micromap != nullptr) {
                micromap_owner = rhi::object_manager<rhi::micromap>{micromap};
                rhi::acceleration_structure_geometry const with_micromap{
                    .vertex_address = source.vertex_address,
                    .vertex_stride = source.vertex_stride,
                    .vertex_count = triangle_vertices,
                    .index_address = source.index_address,
                    .index_format = rhi::index_type::uint32,
                    .index_count = triangle_vertices,
                    .opacity_micromap = micromap,
                };
                rhi::acceleration_structure* const shaded = core->create_acceleration_structure(rhi::acceleration_structure_desc{
                    .type = rhi::acceleration_structure_type::bottom_level,
                    .geometries = &with_micromap,
                    .geometry_count = 1u,
                });
                CHECK(shaded != nullptr);
                if (shaded != nullptr) {
                    shaded_owner = rhi::object_manager<rhi::acceleration_structure>{shaded};
                    CHECK(commands->build_micromap(*micromap) == rhi::error::ok); // ... the micromap FIRST
                    CHECK(commands->barrier(rhi::barrier_group{
                              .has_memory = true,
                              .memory = rhi::memory_barrier{.from = rhi::buffer_use::micromap_write, .to = rhi::buffer_use::micromap_read},
                          }) == rhi::error::ok);
                    CHECK(commands->build_acceleration_structure(*shaded) == rhi::error::ok);
                }
            }
            deren::vk_test::write_line("as_probe: an opacity micromap was created, built and consulted by a geometry");
        }
    }

    CHECK(commands->end_recording() == rhi::error::ok);

    // ---- 6. THE SUBMISSION, THROUGH THE ESCAPE (plan X4's second item: the contract has no submit verb for a
    //         buffer the CALLER owns, which is why this is still a raw pair here).
    {
        VkDevice const device = static_cast<VkDevice>(escape->native_device());
        VkQueue const queue = static_cast<VkQueue>(escape->native_queue());
        VkFenceCreateInfo const fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        VkFence fence = VK_NULL_HANDLE;
        CHECK(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);
        VkSubmitInfo const submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                  .pNext = nullptr,
                                  .waitSemaphoreCount = 0u,
                                  .pWaitSemaphores = nullptr,
                                  .pWaitDstStageMask = nullptr,
                                  .commandBufferCount = 1u,
                                  .pCommandBuffers = &native_commands,
                                  .signalSemaphoreCount = 0u,
                                  .pSignalSemaphores = nullptr};
        CHECK(vkQueueSubmit(queue, 1u, &submit, fence) == VK_SUCCESS);
        CHECK(vkWaitForFences(device, 1u, &fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS); // a hung build fails the probe
        vkDestroyFence(device, fence, nullptr);
        deren::vk_test::write_line("as_probe: built, refit, submitted and waited: 2 bottom level + 1 top level");
    }

    bool native_heap = false;
    for (int index = 1; index < argc; ++index) {
        native_heap = native_heap || std::string_view(argv[index]) == "--native-heap-traversal";
    }
    check_rt_traversal(*core, *escape, *addresses, *capabilities, *blas, *tlas, argv[0], native_heap);

    blas->release();
    static_blas->release();
    tlas->release();
    commands->release();
    vertex_buffer->release();
    index_buffer->release();
    deren::vk_test::write_line("as_probe: done (validation was ON: any VUID appears above)");
    return deren::vk_test::finish("test_acceleration_structures");
}
