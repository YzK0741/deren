// Engine-side RHI error classification; generic utility has no renderer dependency.
export module deren.engine.outcome;
import deren.utility;
import deren.promise.rhi;

namespace deren::engine::outcome {
    /// the outcome of a classification: keep, rewrite, or die. (hopper's shape, deren's error type.)
    ///
    /// `pass_failure` carries a REWRITTEN error, not the raw one: the classifier is the one place
    /// that knows what this failure MEANS for its caller ("acquire said out_of_date -> skip and
    /// rebuild"), and the verdict it hands back is the caller's whole story, raw VkResult gone.
    export struct pass_success {};
    export struct pass_failure {
        deren::promise::rhi::error_info failure;
    };
    export struct fatal {
        deren::promise::rhi::error_info reason;
    };
    export using verdict = std::variant<pass_success, pass_failure, fatal>;

    /**
     * @ingroup engine
     * @brief the engine-side failure type: a value or a fully described failure
     * @note the CONTRACT does not grow an `expected` - its two failure styles (factories answering
     *       nullptr, `command_buffer::use` answering rhi::error) stay what they are; this alias is the
     *       SAME BINARY's richer channel, where deren::utility::vessel::string and a location are free because nothing
     *       crosses a boundary carrying it
     */
    export template <typename value_type>
    using result = std::expected<value_type, deren::promise::rhi::error_info>;

    /**
     * @ingroup engine
     * @brief turn an expected-style outcome into a verdict: success stays success, failure keeps its
     *        error_info - the classifier decides, this only carries
     * @param outcome anything with `operator bool` and `.error()` answering the failure
     *        (std::expected<T, rhi::error_info> is the shape it is for; `result<T>` above)
     */
    export inline constexpr auto propagate = [](auto const& outcome) -> verdict {
        if (outcome) {
            return pass_success{};
        }
        return pass_failure{outcome.error()};
    };

    /**
     * @ingroup engine
     * @brief run an outcome through a classifier and make the verdict STICK
     * @param outcome the engine-side outcome being decided
     * @param classify the one place that knows what failures mean here; answers a verdict
     * @return the outcome, its error REWRITTEN when the classifier said pass_failure
     * @note
     *     - a LYING classifier terminates: judging a failed outcome pass_success, or a successful
     *       one pass_failure, is not a wrong answer - it is a contradiction of the outcome the
     *       classifier was handed, and every decision after it would be made on fiction
     *     - `fatal` is EXECUTED here, in the engine: the backend only reports (device_lost,
     *       out_of_host_memory); the panic that ends the process is the engine's act, at the engine's
     *       one convergence point - which is why the backend's own panic sites are exceptions to
     *       migrate, not a pattern to copy
     *     - the frame path's classifiers land with the frame face (classify_acquire /
     *       classify_present over the frame statuses); today's hand-written if-ladders are the
     *       content this exists to replace
     */
    export template <typename value_type, typename classify_type>
    auto enforce(result<value_type> outcome, classify_type&& classify) -> result<value_type> {
        verdict const decided = classify(outcome);
        if (std::holds_alternative<pass_success>(decided)) {
            if (!outcome) {
                deren::utility::panic("enforce: the classifier judged a FAILED outcome as success", std::source_location::current());
            }
            return outcome;
        }
        if (auto const* const failure = std::get_if<pass_failure>(&decided)) {
            if (outcome) {
                deren::utility::panic("enforce: the classifier judged a SUCCESSFUL outcome as failure", std::source_location::current());
            }
            return std::unexpected(failure->failure); // the rewritten error replaces the raw one
        }
        auto const& reason = std::get<fatal>(decided).reason;
        deren::utility::panic(deren::utility::vessel::format("enforce: fatal outcome {} - {}",
                                                             std::to_underlying(reason.code), reason.message),
                              reason.where);
    }

} // namespace deren::engine::outcome
