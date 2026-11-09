export module deren.asset.subtilis.pages;
export import deren.vstd;
export import deren.asset.subtilis.types;

export namespace deren::asset::subtilis {
    // Offline page planning for the streaming table. `compute_pages` is called by the builder after
    // the DAG is complete; `validate_pages` is the single semantic validator used by the codec, the
    // asset validator and the runtime import path. Both are render-neutral: no page is uploaded or
    // evicted here.
    result<void> compute_pages(payload&, limits const&);
    result<void> validate_pages(payload const&, limits const&);
} // namespace deren::asset::subtilis
