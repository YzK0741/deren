#pragma once
#include <filesystem>
#include <memory>
import deren.asset.subtilis.dag;

namespace deren::tools::subtilis {
    namespace asset = deren::asset::subtilis;
    using asset::result;
    using asset::string;
    using asset::vector;
    using std::byte;
    using path = std::filesystem::path;
    struct primitive_key {
        std::uint32_t mesh = 0, primitive = 0;
        bool operator==(primitive_key const&) const = default;
    };
    struct document_state;
    struct document_deleter {
        void operator()(document_state*) const noexcept;
    };
    struct document {
        std::unique_ptr<document_state, document_deleter> state;
        document();
        ~document();
        document(document&&) noexcept;
        document& operator=(document&&) noexcept;
        document(document const&) = delete;
        document& operator=(document const&) = delete;
    };
    result<document> read_document(path const&, asset::limits const&);
    result<vector<primitive_key>> eligible_primitives(document const&);
    result<asset::source_geometry> extract_geometry(document const&, primitive_key);
    result<void> attach_payload(document&, primitive_key, std::span<byte const>);
    result<vector<byte>> payload_bytes(document const&, primitive_key);
    result<vector<byte>> serialize_embedded_glb(document const&);
    result<string> serialize_json(document const&);
} // namespace deren::tools::subtilis
