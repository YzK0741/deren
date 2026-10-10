#include "vk_test.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <memory>
#include <span>
#include <vector>
import deren.promise.rhi;
import deren.gaussian_loader;
import deren.engine.gaussian_splatting;
namespace gs = deren::engine::gaussian_splatting;
namespace {
    deren::gaussian::asset sample() {
        deren::gaussian::asset a{};
        deren::gaussian::particle p{};
        p.center = {1, 2, 3};
        p.scale = {1, 2, 3};
        p.rotation = {1, 0, 0, 0};
        p.opacity = 0.5f;
        p.sh[0] = 1;
        p.sh[1] = 2;
        p.sh[2] = 3;
        a.particles.push_back(p);
        return a;
    }
    bool close(float a, float b) {
        return std::abs(a - b) < 1e-5f;
    }
    void test_packing() {
        CHECK(sizeof(gs::geometry_record) == 48);
        CHECK(sizeof(gs::sh_record) == 192);
        CHECK(sizeof(gs::instance_record) == 160);
        CHECK(sizeof(gs::draw_reference) == 8);
        auto a = sample();
        auto result = gs::pack_asset(a);
        CHECK(result.has_value());
        if (!result || result->geometry.empty()) {
            CHECK(false);
            return;
        }
        auto const& g = result->geometry[0];
        CHECK((g.center_opacity == std::array<float, 4>({1, 2, 3, 0.5f})));
        CHECK((g.covariance_x == std::array<float, 4>({1, 0, 0, 0})));
        CHECK((g.covariance_yz == std::array<float, 4>({4, 0, 9, 0})));
        CHECK(result->sh[0].coefficients == a.particles[0].sh);
        CHECK((result->bounds_min == std::array<float, 3>({-2, -4, -6})));
        CHECK((result->bounds_max == std::array<float, 3>({4, 8, 12})));
        a.bounds_min = {100, 100, 100};
        CHECK(gs::pack_asset(a)->bounds_min[0] == -2);
        float const q = std::sqrt(0.5f);
        a.particles[0].rotation = {q, 0, 0, q};
        result = gs::pack_asset(a);
        CHECK(close(result->geometry[0].covariance_x[0], 4));
        CHECK(close(result->geometry[0].covariance_yz[0], 1));
        a.particles[0].rotation = {std::cos(0.3926990817f), 0, 0, std::sin(0.3926990817f)};
        result = gs::pack_asset(a);
        CHECK(close(result->geometry[0].covariance_x[0], 2.5f));
        CHECK(close(result->geometry[0].covariance_x[1], -1.5f));
        CHECK(close(result->geometry[0].covariance_yz[0], 2.5f));
        CHECK(close(result->geometry[0].covariance_yz[2], 9));
        CHECK(gs::pack_asset(deren::gaussian::asset{})->geometry.empty());
        a = sample();
        a.particles[0].center = {1, 1, 1};
        a.particles[0].scale = {1e-8f, 1e-8f, 1e-8f};
        result = gs::pack_asset(a);
        CHECK(result);
        CHECK(result->bounds_min[0] < 1.0f);
        CHECK(result->bounds_max[0] > 1.0f);
        a = sample();
        a.particles[0].center = {1e30f, 1e30f, 1e30f};
        a.particles[0].scale = {1e-8f, 1e-8f, 1e-8f};
        result = gs::pack_asset(a);
        CHECK(result);
        if (result) {
            CHECK(result->bounds_min[0] < a.particles[0].center[0]);
            CHECK(result->bounds_max[0] > a.particles[0].center[0]);
        }
        a = sample();
        a.particles[0].rotation = {-1, 0, 0, 0};
        a.particles[0].opacity = 0;
        CHECK(gs::pack_asset(a)->geometry[0].covariance_x[0] == 1);
        a.particles[0].opacity = 1;
        CHECK(gs::pack_asset(a));
        for (std::uint32_t degree = 0; degree <= 3; ++degree) {
            a = sample();
            a.sh_degree = degree;
            unsigned const used = 3 * (degree + 1) * (degree + 1);
            for (unsigned i = 0; i < used; ++i)
                a.particles[0].sh[i] = static_cast<float>(i) + 0.25f;
            for (unsigned i = used; i < 48; ++i)
                a.particles[0].sh[i] = 123.0f;
            result = gs::pack_asset(a);
            CHECK(result && result->sh_degree == degree);
            for (unsigned i = 0; i < 48; ++i)
                CHECK(result->sh[0].coefficients[i] == (i < used ? a.particles[0].sh[i] : 0.0f));
        }
    }
    void test_invalid_packing() {
        auto check = [](auto mutate) {auto a=sample();mutate(a);CHECK(!gs::pack_asset(a)); };
        float const nan = std::numeric_limits<float>::quiet_NaN();
        check([nan](auto& a) { a.particles[0].center[0] = nan; });
        check([](auto& a) { a.particles[0].scale[0] = 0; });
        check([](auto& a) { a.particles[0].scale[0] = -1; });
        check([](auto& a) { a.particles[0].scale[0] = std::numeric_limits<float>::max(); });
        check([](auto& a) { a.particles[0].rotation = {0, 0, 0, 0}; });
        check([](auto& a) { a.particles[0].rotation = {2, 0, 0, 0}; });
        check([](auto& a) { a.particles[0].opacity = -0.1f; });
        check([](auto& a) { a.particles[0].opacity = 1.1f; });
        check([nan](auto& a) { a.particles[0].sh[0] = nan; });
        check([](auto& a) { a.sh_degree = 4; });
        check([nan](auto& a) { a.particles[0].rotation[0] = nan; });
        check([nan](auto& a) { a.particles[0].sh[47] = nan; });
        CHECK(gs::packed_byte_size(1).value() == 240);
        CHECK(gs::packed_byte_size(1, {1, 240}));
        CHECK(!gs::packed_byte_size(1, {1, 239}));
        CHECK(!gs::packed_byte_size(2, {1, 1000}));
        CHECK(!gs::packed_byte_size(std::numeric_limits<std::uint64_t>::max(), {std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()}));
        CHECK(!gs::pack_asset(sample(), {1, 239}));
        CHECK(gs::pack_asset(sample(), {1, 240}));
    }
} // namespace

void test_models() {
    glm::mat4 identity{1};
    CHECK(gs::validate_model(identity));
    auto model = glm::translate(identity, glm::vec3{2, 3, 4});
    model = glm::rotate(model, 0.4f, glm::vec3{0, 0, 1});
    model = glm::scale(model, glm::vec3{2});
    auto inv = gs::validate_model(model);
    CHECK(inv);
    if (inv) {
        auto product = model * *inv;
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned j = 0; j < 4; ++j)
                CHECK(close(product[i][j], i == j ? 1.0f : 0.0f));
    }
    auto bad = identity;
    bad[0][0] = 2;
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[0][0] = -1;
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[1][0] = 0.25f;
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[0][3] = 0.01f;
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[3][3] = 2;
    CHECK(!gs::validate_model(bad));
    bad = glm::mat4{0};
    CHECK(!gs::validate_model(bad));
    bad = glm::scale(identity, glm::vec3{std::numeric_limits<float>::denorm_min()});
    CHECK(!gs::validate_model(bad));
    bad = identity;
    bad[3][0] = std::numeric_limits<float>::infinity();
    CHECK(!gs::validate_model(bad));
}
void test_sorting() {
    auto a = sample();
    a.particles[0].center = {0, 0, -1};
    a.particles.push_back(a.particles[0]);
    a.particles[1].center[2] = -3;
    std::array<gs::sort_instance, 2> instances = {gs::sort_instance{&a, glm::mat4{1}, 20}, gs::sort_instance{&a, glm::translate(glm::mat4{1}, glm::vec3{0, 0, -1}), 10}};
    auto sorted = gs::sort_draw_references(instances, {});
    CHECK(sorted && sorted->size() == 4);
    if (sorted && sorted->size() == 4) {
        CHECK((*sorted)[0].instance_index == 1 && (*sorted)[0].local_gaussian_index == 1);
        CHECK((*sorted)[1].instance_index == 0 && (*sorted)[1].local_gaussian_index == 1);
        CHECK((*sorted)[2].instance_index == 1 && (*sorted)[2].local_gaussian_index == 0);
        CHECK((*sorted)[3].instance_index == 0 && (*sorted)[3].local_gaussian_index == 0);
    }
    sorted = gs::sort_draw_references(instances, {{0, 0, 0}, {0, 0, 1}});
    CHECK(sorted && sorted->size() == 4);
    if (sorted && !sorted->empty())
        CHECK((*sorted)[0].instance_index == 0 && (*sorted)[0].local_gaussian_index == 0);
    instances[1].model = glm::mat4{1};
    a.particles[1].center[2] = -1;
    sorted = gs::sort_draw_references(instances, {});
    if (sorted && sorted->size() == 4) {
        CHECK((*sorted)[0].instance_index == 1 && (*sorted)[0].local_gaussian_index == 0);
        CHECK((*sorted)[1].instance_index == 1 && (*sorted)[1].local_gaussian_index == 1);
        CHECK((*sorted)[2].instance_index == 0 && (*sorted)[2].local_gaussian_index == 0);
    }
    std::swap(instances[0], instances[1]);
    sorted = gs::sort_draw_references(instances, {});
    CHECK(sorted && sorted->size() == 4);
    if (sorted && sorted->size() == 4) {
        CHECK((*sorted)[0].instance_index == 0);
        CHECK((*sorted)[0].local_gaussian_index == 0);
        CHECK((*sorted)[1].local_gaussian_index == 1);
    }
    std::swap(instances[0], instances[1]);
    auto overflow = instances;
    overflow[0].model = glm::scale(glm::mat4{1}, glm::vec3{std::numeric_limits<float>::max()});
    a.particles[0].center[2] = -2;
    CHECK(!gs::sort_draw_references(overflow, {}));
    a.particles[0].center[2] = -1;
    CHECK(gs::sort_draw_references(instances, {}, {4, 32}));
    CHECK(!gs::sort_draw_references(instances, {}, {3, 32}));
    CHECK(!gs::sort_draw_references(instances, {}, {4, 31}));
    instances[1].stable_id = 20;
    CHECK(!gs::sort_draw_references(instances, {}));
    instances[1].stable_id = 10;
    instances[1].asset = nullptr;
    CHECK(!gs::sort_draw_references(instances, {}));
    instances[1].asset = &a;
    CHECK(!gs::sort_draw_references(instances, {{0, 0, 0}, {0, 0, 0}}));
    CHECK(!gs::sort_draw_references(instances, {{0, 0, 0}, {0, 0, -2}}));
    CHECK(!gs::sort_draw_references(instances, {{std::numeric_limits<float>::infinity(), 0, 0}, {0, 0, -1}}));
    a.particles[0].center[2] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!gs::sort_draw_references(instances, {}));
    a.particles.clear();
    CHECK(gs::sort_draw_references(instances, {})->empty());
    instances[1].stable_id = 20;
    CHECK(!gs::sort_draw_references(instances, {}));
    CHECK(gs::sort_draw_references({}, {})->empty());
}

namespace rhi = deren::promise::rhi;
struct upload_probe;
struct probe_buffer : rhi::buffer {
    unsigned index;
    int& releases;
    std::uint64_t capacity;
    probe_buffer(unsigned i, int& r, std::uint64_t size)
        : buffer("gaussian_probe_buffer")
        , index(i)
        , releases(r)
        , capacity(size) {
    }
    void release() noexcept override {
        ++releases;
        delete this;
    }
    std::uint64_t size() const noexcept override {
        return capacity;
    }
    std::span<std::byte> mapped() noexcept override {
        return {};
    }
};
struct probe_address : rhi::device_address {
    unsigned zero_at = 0, unaligned_at = 0;
    probe_address()
        : device_address("gaussian_probe_address") {
    }
    std::uint64_t buffer_address(rhi::buffer const& b, std::uint64_t) const noexcept override {
        auto const& buffer = static_cast<probe_buffer const&>(b);
        return buffer.index == zero_at ? 0 : buffer.index * 4096ull + (buffer.index == unaligned_at ? 1 : 0);
    }
};
struct upload_probe : rhi::api_core {
    bool supported = true;
    unsigned fail_at = 0, calls = 0, undersized_at = 0;
    int releases = 0;
    probe_address address;
    std::vector<rhi::buffer_desc> descriptors;
    std::vector<std::vector<std::byte>> bytes;
    upload_probe()
        : api_core("gaussian_upload_probe") {
    }
    rhi::ability_bits abilities() const noexcept override {
        return supported ? rhi::to_bits(rhi::extension_kind::device_address) : 0;
    }
    rhi::extension* query_extension(rhi::extension_kind kind) noexcept override {
        return supported && kind == rhi::extension_kind::device_address ? &address : nullptr;
    }
    rhi::buffer* create_buffer(rhi::buffer_desc const& desc) override {
        ++calls;
        if (calls == fail_at)
            return nullptr;
        auto copy = desc;
        copy.initial_bytes = {};
        descriptors.push_back(copy);
        bytes.emplace_back(desc.initial_bytes.begin(), desc.initial_bytes.end());
        return new probe_buffer(calls, releases, calls == undersized_at ? desc.size - 1 : desc.size);
    }
    rhi::swapchain* create_swapchain(rhi::swapchain_desc const&) override {
        return nullptr;
    }
    rhi::image* create_image(rhi::image_desc const&) override {
        return nullptr;
    }
    rhi::acceleration_structure* create_acceleration_structure(rhi::acceleration_structure_desc const&) override {
        return nullptr;
    }
    rhi::micromap* create_micromap(rhi::micromap_desc const&) override {
        return nullptr;
    }
    rhi::sampler* create_sampler(rhi::sampler_desc const&) override {
        return nullptr;
    }
    rhi::shader* create_shader(rhi::shader_desc const&) override {
        return nullptr;
    }
    rhi::pipeline* create_pipeline(rhi::pipeline_desc const&) override {
        return nullptr;
    }
    rhi::query* create_query(rhi::query_desc const&) override {
        return nullptr;
    }
    rhi::command_buffer* begin_commands() override {
        return nullptr;
    }
    rhi::image* frame_image() noexcept override {
        return nullptr;
    }
    rhi::buffer* frame_readback_buffer() noexcept override {
        return nullptr;
    }
    rhi::submit_info frame_begin() override {
        return {};
    }
    rhi::error present() override {
        return rhi::error::unsupported;
    }
    void wait_idle() override {
        CHECK(false);
    }
    rhi::frame_walker* walk_frames() noexcept override {
        return nullptr;
    }
    rhi::gpu_profiler* profiler() noexcept override {
        return nullptr;
    }
    rhi::swapchain* frame_swapchain() noexcept override {
        return nullptr;
    }
    rhi::error submit(rhi::command_buffer&) override {
        return rhi::error::unsupported;
    }
    rhi::command_buffer* create_command_buffer(rhi::command_buffer_desc const&) override {
        return nullptr;
    }
    std::shared_ptr<rhi::command_buffer> make_command_buffer(rhi::command_buffer_desc const&) override {
        return {};
    }
    std::uint32_t api_version() const noexcept override {
        return rhi::abi_version;
    }
};
void test_upload() {
    auto packed = gs::pack_asset(sample());
    CHECK(packed);
    if (!packed)
        return;
    upload_probe probe;
    auto result = gs::upload_asset(probe, *packed);
    CHECK(result && *result);
    if (result && *result) {
        CHECK((*result)->particle_count() == 1);
        CHECK((*result)->byte_size() == 240);
        CHECK((*result)->sh_degree() == 0);
        CHECK((*result)->geometry_address() == 4096);
        CHECK((*result)->sh_address() == 8192);
        CHECK((*result)->bounds_min() == packed->bounds_min);
        CHECK((*result)->bounds_max() == packed->bounds_max);
        CHECK(probe.descriptors.size() == 2);
        CHECK(probe.descriptors[0].size == 48);
        CHECK(probe.descriptors[1].size == 192);
        for (auto const& d : probe.descriptors) {
            CHECK(d.usage == rhi::buffer_usage::storage_gpu_only);
            CHECK(rhi::has_flag(d.flags, rhi::buffer_flag::storage));
            CHECK(rhi::has_flag(d.flags, rhi::buffer_flag::device_address));
        }
        CHECK(std::memcmp(probe.bytes[0].data(), packed->geometry.data(), 48) == 0);
        CHECK(std::memcmp(probe.bytes[1].data(), packed->sh.data(), 192) == 0);
        auto shared = *result;
        result->reset();
        CHECK(probe.releases == 0);
        shared.reset();
        CHECK(probe.releases == 2);
    }
    upload_probe absent;
    absent.supported = false;
    auto failed = gs::upload_asset(absent, *packed);
    CHECK(!failed && failed.error().code == gs::error_code::unsupported);
    CHECK(absent.calls == 0);
    for (unsigned i = 1; i <= 2; ++i) {
        upload_probe failing;
        failing.fail_at = i;
        failed = gs::upload_asset(failing, *packed);
        CHECK(!failed && failed.error().code == gs::error_code::allocation_failure);
        CHECK(failing.releases == static_cast<int>(i) - 1);
    }
    for (unsigned i = 1; i <= 2; ++i) {
        upload_probe zero;
        zero.address.zero_at = i;
        failed = gs::upload_asset(zero, *packed);
        CHECK(!failed && failed.error().code == gs::error_code::invalid_address);
        CHECK(zero.releases == static_cast<int>(i));
    }
    for (unsigned i = 1; i <= 2; ++i) {
        upload_probe short_buffer;
        short_buffer.undersized_at = i;
        failed = gs::upload_asset(short_buffer, *packed);
        CHECK(!failed && failed.error().code == gs::error_code::allocation_failure);
        CHECK(short_buffer.releases == static_cast<int>(i));
    }
    for (unsigned i = 1; i <= 2; ++i) {
        upload_probe unaligned;
        unaligned.address.unaligned_at = i;
        failed = gs::upload_asset(unaligned, *packed);
        CHECK(!failed && failed.error().code == gs::error_code::invalid_address);
        CHECK(unaligned.releases == static_cast<int>(i));
    }
    upload_probe empty;
    empty.supported = false;
    auto nothing = gs::pack_asset(deren::gaussian::asset{});
    result = gs::upload_asset(empty, *nothing);
    CHECK(result && *result);
    if (result && *result) {
        CHECK((*result)->particle_count() == 0);
        CHECK((*result)->geometry_address() == 0);
        CHECK((*result)->sh_address() == 0);
    }
    CHECK(empty.calls == 0);
    auto bad = [&](auto mutate) {auto p=*packed;mutate(p);upload_probe check;auto invalid=gs::upload_asset(check,p);CHECK(!invalid && invalid.error().code==gs::error_code::invalid_argument);CHECK(check.calls==0); };
    bad([](auto& p) { p.sh.clear(); });
    bad([](auto& p) { p.sh_degree = 4; });
    bad([](auto& p) { p.geometry[0].center_opacity[3] = 2; });
    bad([](auto& p) { p.geometry[0].covariance_x[0] = -1; });
    bad([](auto& p) { p.geometry[0].covariance_x[1] = 100; });
    bad([](auto& p) { p.sh[0].coefficients[0] = std::numeric_limits<float>::quiet_NaN(); });
    bad([](auto& p) { p.geometry[0].covariance_x[3] = 1; });
    bad([](auto& p) { p.bounds_min[0] = 100; });
    bad([](auto& p) { p.sh[0].coefficients[47] = 1; });
    bad([](auto& p) { p.geometry[0].center_opacity[0] = std::numeric_limits<float>::infinity(); });
    bad([](auto& p) { p.bounds_max[0] = std::numeric_limits<float>::quiet_NaN(); });
    upload_probe narrow;
    auto manual = *packed;
    manual.bounds_min = {1, 2, 3};
    manual.bounds_max = {1, 2, 3};
    auto rebuilt = gs::upload_asset(narrow, manual);
    CHECK(rebuilt && *rebuilt);
    if (rebuilt && *rebuilt) {
        CHECK((*rebuilt)->bounds_min() == packed->bounds_min);
        CHECK((*rebuilt)->bounds_max() == packed->bounds_max);
    }
    upload_probe limited;
    failed = gs::upload_asset(limited, *packed, {1, 239});
    CHECK(!failed && failed.error().code == gs::error_code::limit_exceeded);
    CHECK(limited.calls == 0);
}
int main() {
    test_packing();
    test_invalid_packing();
    test_models();
    test_sorting();
    test_upload();
    return deren::vk_test::finish("gaussian_splatting");
}