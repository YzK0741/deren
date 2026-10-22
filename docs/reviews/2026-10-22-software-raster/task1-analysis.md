# Task 1 software-raster workload upper bound (GO/NO-GO)

GO: **YES**  (far gate: {'go': True, 'gate_ms': 0.30165, 'delta_ms': 1.5399999999999991, 'delta_pct_of_m1c': 7.657881650919936})

| camera | round | baseline raster ms | holes raster ms | delta ms | delta % M1c | GPU total delta ms | workgroups removed | probe sw clusters | tile max 1.0/2.0 |
|---|---|---|---|---|---|---|---|---|---|
| far | 1 | 19.18 | 17.64 | 1.54 | 7.66% | 1.51 | 554523 | 4233 | 977/1907 |
| far | 2 | 19.65 | 18.13 | 1.52 | 7.56% | 1.54 | 554523 | 4233 | 977/1907 |
| far | 3 | 20.00 | 17.72 | 2.28 | 11.34% | 2.29 | 554523 | 4233 | 977/1907 |
| **far median** |  | 19.65 | 17.72 | 1.54 | 7.66% | 1.54 | 554523 | 4233 |  |
| near | 1 | 17.97 | 16.61 | 1.36 | 7.32% | 1.48 | 111350 | 850 | 226/290 |
| near | 2 | 17.30 | 17.21 | 0.09 | 0.48% | 0.09 | 111350 | 850 | 226/290 |
| near | 3 | 16.97 | 16.92 | 0.05 | 0.27% | -0.10 | 111350 | 850 | 226/290 |
| **near median** |  | 17.30 | 16.92 | 0.09 | 0.48% | 0.09 | 111350 | 850 |  |

- **far**: classify 1.0 px share 10.32% of clusters (M1c probe 23.28%), 2.0 px 26.62% (probe 38.23%); triangle shares 8.96%/25.27% (probe 21.92%/36.75%); CPU classify 7935.6 us work/frame (session avg 2748.0 us), capture-frame binning 4095.9 us (primary 2131.1 + secondary 1986.8; capture frame only, host copies).
- **near**: classify 1.0 px share 2.40% of clusters (M1c probe 5.37%), 2.0 px 6.44% (probe 15.95%); triangle shares 1.37%/5.17% (probe 4.12%/14.46%); CPU classify 4256.8 us work/frame (session avg 2211.6 us), capture-frame binning 3401.9 us (primary 1725.4 + secondary 1649.2; capture frame only, host copies).

tile entry capacity (Q7): max entries 1.0 px = 977, 2.0 px = 1907; overflow tiles@32/64 = 33/18 (1.0 px) -> **capacity 1024** (measured per-tile maximum at 1.0 px is 977 entries: NEITHER 32 NOR 64 holds it (Q7's two candidates are both insufficient); the smallest power of two that does not overflow every frame is 1024. The 2.0 px band would need 2048 (max 1907).)
