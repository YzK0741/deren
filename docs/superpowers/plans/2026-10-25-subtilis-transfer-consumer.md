# Subtilis transfer consumer plan

Use inline execution with an independent review at completion. Spec: ../specs/2026-10-25-subtilis-transfer-consumer.md.

- [x] Add engine upload-mode declaration and a failing forced-batch capability test against the CPU probe.
- [x] Implement allocation-only generations, owning aliases, one batch and targeted wait before publication.
- [x] Add device consumer probe with byte readback and repeated synchronous/batched latency measurements.
- [x] Run full build at 14 workers, CTest and device lane; review and document limits/results.

Evidence and limits: ../../reviews/2026-10-25-subtilis-transfer-consumer.md.
