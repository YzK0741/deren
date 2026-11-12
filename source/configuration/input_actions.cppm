export module deren.input_actions;

import deren.vstd;
import deren.utility.shared;

export namespace deren::input {
    using key = std::uint8_t;
    enum named_key : key {
        unknown = 0,
        left,
        right,
        up,
        down_arrow,
        enter,
        tab,
        left_shift,
        right_shift,
        left_control,
        right_control,
        space = ' '
    };

    enum action_flag : std::uint8_t {
        down = 1,
        pressed = 2,
        released = 4
    };

    struct action_binding {
        utility::vessel::string name;
        utility::vessel::vector<key> keys;
    };

    [[nodiscard]] inline key key_from_name(std::string_view name) {
        if (name.size() == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'))) {
            return static_cast<key>(name[0]);
        }
        constexpr std::pair<std::string_view, key> names[]{
            {"space", space}, {"left", left}, {"right", right}, {"up", up}, {"down", down_arrow}, {"enter", enter}, {"tab", tab}, {"left_shift", left_shift}, {"right_shift", right_shift}, {"left_control", left_control}, {"right_control", right_control}};
        for (auto const& [label, value] : names) {
            if (label == name) {
                return value;
            }
        }
        return unknown;
    }

    class action_state {
    public:
        explicit action_state(std::span<action_binding const> bindings)
            : bindings(bindings) {
            pending.resize(bindings.size(), 0);
            frame.resize(bindings.size(), 0);
        }

        void set_key(key value, bool held) {
            if (value == unknown || value >= keys.size() || keys[value] == held) {
                return;
            }
            keys[value] = held;
            for (std::size_t index = 0; index < bindings.size(); ++index) {
                auto const& binding = bindings[index];
                if (std::ranges::find(binding.keys, value) == binding.keys.end()) {
                    continue;
                }
                bool const action_held = std::ranges::any_of(binding.keys, [this](key bound) { return keys[bound]; });
                auto& flags = pending[index];
                if (action_held && (flags & down) == 0) {
                    flags |= down | pressed;
                } else if (!action_held && (flags & down) != 0) {
                    flags = static_cast<std::uint8_t>((flags & ~down) | released);
                }
            }
        }

        void release_keys() {
            keys.fill(false);
            for (auto& flags : pending) {
                if ((flags & down) != 0) {
                    flags = static_cast<std::uint8_t>((flags & ~down) | released);
                }
            }
        }

        // Events arriving while the renderer retries stay pending for the next script tick.
        [[nodiscard]] std::optional<std::span<std::uint8_t const>> begin_frame() {
            if (begun) {
                return std::nullopt;
            }
            begun = true;
            for (std::size_t index = 0; index < pending.size(); ++index) {
                frame[index] = pending[index];
                pending[index] &= down;
            }
            return std::span<std::uint8_t const>{frame.data(), frame.size()};
        }

        void finish_frame() {
            begun = false;
        }

    private:
        std::span<action_binding const> bindings;
        std::array<bool, 128> keys{};
        utility::vessel::vector<std::uint8_t> pending;
        utility::vessel::vector<std::uint8_t> frame;
        bool begun = false;
    };
} // namespace deren::input
