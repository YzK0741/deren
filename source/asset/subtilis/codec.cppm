export module deren.asset.subtilis.codec;
export import deren.asset.subtilis.types;

export namespace deren::asset::subtilis {
    // Wire validation only; decoded payload is untrusted until make_asset succeeds.
    result<vector<byte>> encode(payload const&, limits const&);
    result<payload> decode(std::span<byte const>, limits const&);
    digest sha256(std::span<byte const>);
    result<digest> source_digest(source_geometry const&);
    result<digest> build_digest(build_options const&);
} // namespace deren::asset::subtilis
