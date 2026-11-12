// Headless unit tests: utility ECS ===========================================
// Covers generation-checked entity handles, sparse-set component storage, replacement semantics,
// registry-wide cleanup, and type ids.
#include "vk_test.h"

#include <cstdint>

import deren.utility;

using deren::utility::ecs::component_traits;
using deren::utility::ecs::entity;
using deren::utility::ecs::registry;

namespace {
    struct position {
        float x = 0.0F;
        float y = 0.0F;
    };

    struct velocity {
        float x = 0.0F;
        float y = 0.0F;
    };

    struct no_default {
        explicit no_default(int value_in)
            : value(value_in) {
        }
        int value = 0;
    };

} // namespace

struct tagged {
    int value = 0;
};

namespace deren::utility::ecs {
    template <>
    struct component_traits<tagged> {
        static constexpr std::uint64_t id = 0x9e3779b97f4a7c15ULL;
    };
} // namespace deren::utility::ecs

namespace {
    void test_entity_lifecycle() {
        registry world;

        entity const first = world.create();
        CHECK(first.valid());
        CHECK(world.alive(first));
        CHECK(world.size() == 1);

        CHECK(world.destroy(first));
        CHECK(!world.alive(first));
        CHECK(world.size() == 0);
        CHECK(!world.destroy(first));

        entity const reused = world.create();
        CHECK(reused.valid());
        CHECK(reused.index == first.index);
        CHECK(reused.generation != first.generation);
        CHECK(!world.alive(first));
        CHECK(world.alive(reused));
    }

    void test_component_queries() {
        registry world;
        entity const value = world.create();

        position* const inserted = world.try_emplace<position>(value, 1.0F, 2.0F);
        CHECK(inserted != nullptr);
        CHECK(world.has<position>(value));
        CHECK(world.get<position>(value).x == 1.0F);
        CHECK(world.get<position>(value).y == 2.0F);
        CHECK(world.try_get<position>(value) == inserted);

        CHECK(world.try_emplace<position>(value, 3.0F, 4.0F) == nullptr);
        CHECK(world.get<position>(value).x == 1.0F);

        CHECK(world.emplace_or_replace<position>(value, 3.0F, 4.0F) != nullptr);
        CHECK(world.get<position>(value).x == 3.0F);
        CHECK(world.get<position>(value).y == 4.0F);

        CHECK(world.remove<position>(value));
        CHECK(!world.has<position>(value));
        CHECK(!world.remove<position>(value));

        CHECK(world.try_emplace<no_default>(value, 7) != nullptr);
        CHECK(world.get<no_default>(value).value == 7);

        CHECK(world.try_emplace<tagged>(value, 11) != nullptr);
        CHECK(world.get<tagged>(value).value == 11);
        CHECK(component_traits<tagged>::id == 0x9e3779b97f4a7c15ULL);
        CHECK(component_traits<position>::id != component_traits<velocity>::id);

        entity const stale{value.index, value.generation + 1};
        CHECK(world.try_emplace<position>(stale, 5.0F, 6.0F) == nullptr);
        CHECK(world.try_get<position>(stale) == nullptr);
    }

    void test_swap_and_pop() {
        registry world;
        entity const first = world.create();
        entity const middle = world.create();
        entity const last = world.create();

        CHECK(world.try_emplace<position>(first, 1.0F, 0.0F) != nullptr);
        CHECK(world.try_emplace<position>(middle, 2.0F, 0.0F) != nullptr);
        CHECK(world.try_emplace<position>(last, 3.0F, 0.0F) != nullptr);

        CHECK(world.remove<position>(middle));
        CHECK(world.has<position>(first));
        CHECK(!world.has<position>(middle));
        CHECK(world.has<position>(last));
        CHECK(world.get<position>(last).x == 3.0F);
        CHECK(world.get<position>(first).x == 1.0F);
    }

    void test_destroy_clears_components() {
        registry world;
        entity const value = world.create();

        CHECK(world.try_emplace<position>(value, 1.0F, 2.0F) != nullptr);
        CHECK(world.try_emplace<velocity>(value, 3.0F, 4.0F) != nullptr);

        CHECK(world.destroy(value));
        CHECK(!world.alive(value));
        CHECK(!world.has<position>(value));
        CHECK(!world.has<velocity>(value));
        CHECK(world.try_get<position>(value) == nullptr);
    }

    void test_iteration() {
        registry world;
        entity const first = world.create();
        entity const second = world.create();
        entity const without_position = world.create();

        CHECK(world.try_emplace<position>(first, 1.0F, 0.0F) != nullptr);
        CHECK(world.try_emplace<position>(second, 2.0F, 0.0F) != nullptr);

        std::uint32_t count = 0;
        float sum = 0.0F;
        world.each<position>([&](entity, position& value) {
            ++count;
            sum += value.x;
        });
        CHECK(count == 2);
        CHECK(sum == 3.0F);

        count = 0;
        world.each_entity([&](entity value) {
            CHECK(world.alive(value));
            ++count;
        });
        CHECK(count == 3);
        CHECK(world.alive(without_position));

        registry const& const_world = world;
        count = 0;
        const_world.each<position>([&](entity, position const& value) {
            CHECK(value.x > 0.0F);
            ++count;
        });
        CHECK(count == 2);
    }

    void test_clear() {
        registry world;
        entity const value = world.create();
        CHECK(world.try_emplace<position>(value, 1.0F, 2.0F) != nullptr);

        world.clear();
        CHECK(world.size() == 0);
        CHECK(!world.alive(value));
        CHECK(world.try_get<position>(value) == nullptr);

        entity const reused = world.create();
        CHECK(reused.index == value.index);
        CHECK(reused.generation != value.generation);
        CHECK(!world.alive(value));
        CHECK(world.alive(reused));
        CHECK(world.try_get<position>(reused) == nullptr);
    }
} // namespace

int main() {
    test_entity_lifecycle();
    test_component_queries();
    test_swap_and_pop();
    test_destroy_clears_components();
    test_iteration();
    test_clear();
    return deren::vk_test::finish("test_ecs");
}
