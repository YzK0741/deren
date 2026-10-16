# A1 execution ledger

Baseline: caac9f65; user authorized working in the existing checkout. Existing uncommitted changes preserved. No commits or pushes authorized.

Preflight: plan tasks 1-4 map to spec A1 only; Boost 1.92.0 and allocator probe already validated. Dependencies: Task1 -> Task2 -> Task3 -> Task4.

Ruling: types.cppm is an internal partition deren.engine.render_graph:types, rather than a separate named module. Opaque refs and builder friendship then belong to the same named module. Public import remains deren.engine.render_graph; cost if wrong: change internal partition boundary, no current external consumers.
Ruling: shared BGL adapter needs a small global-fragment headers file plus a post-import body header, because utility is a C++ module and its allocator types cannot be referenced before import. Cost if wrong: one internal include split; tests reuse the same body.
Task 1: complete. RED: missing deren.engine.render_graph module; GREEN: focused CTest 1/1, registration/identity/ref/name/freeze checks passed. No commit; user-owned changes preserved.
Task 2: complete. RED: 12 content/version/culling assertions failed against registration-only compiler; GREEN: test_render_graph and shared-adapter test_boost_graph 2/2 passed. BGL adapter extraction advanced from Task3 because Task2 reverse traversal consumes it. No production/RHI changes.
Task 3: complete. RED: 8 hazard/explicit-order assertions failed; GREEN: test_render_graph and test_boost_graph 2/2 passed. Live graph carries value/RAW/WAR/WAW/explicit reasons; structural order cycles checked independently of culling.
Ruling: move shared backend extraction into Task2 because value-root traversal needs it there; Task3 then reuses the same adapter. Cost if wrong: task boundary only, no interface difference.
Task 4 verification: diagnostic-name RED reproduced1failure then GREEN; Release full32/32, ASan/UBSan focused3/3. Graph58checks; Boost274checks including actual module, ordinary new delta0. Artifact backend boundary explicitly measures new core archive and is zero. Final independent review pending.
Task 4: complete. Full Release32/32, ASan/UBSan3/3, backend/native boundary zero. Independent reviewer found no actionable correctness/integration defects and no deferred minors. No fix pass needed. Plan steps checked; A2/B/C remain pending. No commits/pushes or old mechanism removal.
Final formatting gate: two private headers normalized by clang-format; Release focused4/4 and rebuilt ASan/UBSan3/3 passed afterward. No semantic changes or additional review findings.
