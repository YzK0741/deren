# Error handling and failure policy

Every fallible operation answers three questions: how the failure is represented, who decides what it
means, and what ends the process. This document records the answers as implemented today, the policy
new code is expected to follow, and the gaps between the two.

## What a caller can get today

| style | where it is used | measured size |
| --- | --- | --- |
| `rhi::error` | the RHI contract's own enum; every backend call that can fail answers one. It is the boundary's vocabulary, and the contract deliberately does not grow `std::expected` (a factory answers nullptr, the command verbs answer `rhi::error`) | the contract's whole fallible surface |
| `std::expected<void, deren::utility::vessel::string>` | the engine's internal workhorse: the pipeline builders in `runtime`, the mask-bake and compute-skin jobs, the acceleration-structure build, the resource-schema validation, the glTF sidecars, `utility::write_png` | 26 declarations plus their definitions |
| `std::expected<T, deren::promise::rhi::error_info>` | the code-bearing answer, written directly - there is no alias or wrapper module for it any more. `error_info` carries the code, the backend's message and its source location | intended for APIs whose callers must classify; not started in production |
| `bool` / `frame_status` | the frame loop's coarse result: `proceed`, `skipped`, `closed`, and one status per failed verb. Used where the caller's action (continue, yield, exit) is all that matters | the frame loop |
| `panic` / `fast_panic` | "stop now": log the message and the raw stack, then terminate | 38 sites in `source/engine` |

## The policy

1. **The contract boundary answers `rhi::error`.** No engine type crosses that boundary; the backend
   only reports. Its comments are the source of truth for which codes are recoverable
   (`out_of_date`, `out_of_device_memory`), fatal (`device_lost`, `out_of_host_memory`), or a caller
   mistake (`invalid_argument`).
2. **A failure a caller must CLASSIFY answers `std::expected<T, rhi::error_info>`.** A failure a
   caller only REPORTS may keep the string-only expected. Do not invent a third representation: the
   two expected forms differ by whether the code is available, which is exactly the difference in what
   the caller can do about it.
3. **A failure nobody can act on is logged exactly once and re-raised.** Never swallowed, never
   turned into a default value: a fallback that hides a failure is the same bug as a crash, one frame
   later.
4. **`panic` is for programming errors only**: a caller that violated the subsystem's contract
   (`get_scene()` before `set_scene()`, a material table that must start with the default material, a
   stream that ends early). Those are not environment-dependent and the message is the contract.
5. **Environment-caused fatals are not programming errors.** A required pipeline that cannot be
   created at startup, a lost device, exhausted host memory: today these also go through `panic`, with
   a message naming the resource. A single funnel for them is listed as a gap below, not as done.
6. **A refusal is not a pass.** A test that cannot exercise its subject returns
   `deren::vk_test::skip_code` (77) and CTest reports Skipped, so green always means the assertions
   ran (see `source/tests/vk_test.h`).

## Decision: there is no verdict family

A `deren.engine.outcome` module used to carry `result`, `verdict`, `propagate` and `enforce`: a
classifier answered `pass_success` / `pass_failure` / `fatal`, and `enforce` executed the fatal and
rewrote the error. It was deleted after measurement:

- its only production consumer was the frame-open path in `runtime.frames.cppm`;
- `result` was a one-line alias of `std::expected<T, rhi::error_info>`, so it added a module import
  and a link target for no expressiveness;
- the "lying classifier" guard protects against a classifier contradicting its own outcome, which a
  total function over the expected cannot do;
- the `verdict` vocabulary had no second user to share it with.

That path now applies the same policy inline (fatal -> panic, `out_of_date` -> rebuild and skip,
anything else -> `acquire_failed`). The rule for reintroducing an abstraction like it: wait until a
SECOND classifier with a DIFFERENT policy exists, because then the vocabulary has something to share.

## Gaps

- **38 panic sites**, each its own policy decision. "Which startup failures end the process?" cannot
  be answered from one place; it needs reading them.
- **`out_of_device_memory` is documented RECOVERABLE and has no recovery path.** In practice a failed
  allocation during construction is a panic, so a recoverable code behaves as a fatal one.
- **The string-only expected carries no code** (26 declarations), so recovery options are invisible
  at the point where the failure is produced.
- **No single crash hook.** The fatal path is a good place for crash reporting - the raw-stack
  machinery in [stacktrace.md](stacktrace.md) already exists - but it is reached from many places.

## Migration status

| step | state |
| --- | --- |
| failure types live in the contract | yes: `rhi::error` and `error_info` |
| one classification mechanism | deleted as unused; the policy is inline at the one call site |
| environment fatals behind one funnel | not done: 38 panic sites |
| code-bearing answers for classify-able APIs | not started: 26 string-only declarations |
| recovery for `out_of_device_memory` | not designed |

## Rules for new code

- A new fallible engine API whose callers must classify returns
  `std::expected<T, deren::promise::rhi::error_info>`.
- A new call site classifies the code and keeps the ACTION there (rebuild, skip, fail): policy does
  not belong inside a shared helper.
- A new `panic` is only for a contract violation, and its message names the contract it enforces.
- Do not log-and-continue on a failure whose result is used; do not fold a failure into a default.
- When a test cannot reach its subject, skip (exit code 77) rather than assert nothing.
