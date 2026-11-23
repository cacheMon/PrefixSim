# Reuse locality: the share of reuse that crosses sessions

`traceReuseStat` reports **sigma_cross**, the fraction of reuse accesses whose
previous accessor belonged to a different session. This is a **locality
descriptor**, not proof that any eviction policy must win. Cache capacity,
access ordering, pinned blocks and compute costs also matter.

The quantities it prints:

| key | meaning |
|---|---|
| `first_touch_share` | accesses that are not reuses |
| `reuse_share` | accesses that are reuses |
| `reuse_same_session_share` | of reuses, the part whose previous accessor is the same session |
| `reuse_cross_session_share` | of reuses, the part that crosses a session boundary |
| `sigma_cross` | `reuse_cross_session_share`, i.e. what fraction of the policy's budget is spent on cross-session reuse |
| `cross_session_gap_p50_s`, `_p90_s`, `_mean_s` | how long a cross-session reuse waits, in seconds of trace time |
| `reuse_cross_user` | accesses whose previous accessor is another user (0 when the trace has no user ids) |

`--gap-hist` adds log2 buckets of the cross-session gap, and
`--per-request-out FILE` writes per-request counts for a time-series view.
### Streaming memory and quantile accuracy

The previous implementation retained every reuse gap in large vectors and
then copied them for sorting. The replacement uses a **fixed-memory**
deterministic quantile summary: exact through 4,096 gaps; otherwise a
64-subdivisions-per-octave histogram with **<=1/64 relative bucket width**
for positive gaps. Printed `*_interval_s` bounds contain each reported
p50/p90; `gap_quantile_method` specifies when the output is approximate.
The `--gap-hist` histogram remains exact, and nonmonotone/nonfinite gaps
cause an error. Two fixed histograms together occupy roughly 2.1 MB.

Last-access state still grows with **distinct block IDs**. Dense interned
prefix IDs use vectors; sparse raw hash IDs use a map, avoiding allocation
based on the numeric magnitude of a hash. The production-scale example
will still require substantial memory for its distinct-block table.

Requested requests are ordered by completion time, so the first accessor of a
block is the accessor that registered it, exactly as the simulator replays it.

## Measurements

`freeinference.lcp.zst` from [harvardMadsys/2026_prefixcache](https://huggingface.co/datasets/harvardMadsys/2026_prefixcache)
(327,548 requests, 656,240,179 block accesses, 39,578,367 distinct blocks,
222,934 sessions, 7 days):

```
$ traceReuseStat freeinference.lcp.zst
requests=327548
block_accesses=656240179
distinct_blocks=39578367
first_touch_share=0.0603
reuse_share=0.9397
reuse_same_session_share=0.8302
reuse_cross_session_share=0.1698
sigma_cross=0.1698
cross_session_gap_mean_s=404
```

Four reuses in five stay inside a session and a cross-session reuse comes back
after ~7 minutes. Replaying that trace at 65,536 blocks gives LRU 0.917540,
SIEVE 0.917670 and S3FIFO 0.912296: at this locality, victim choice is worth
0.014 percentage points, and an admission/frequency policy loses 0.57.

The other end of the scale is a conversation workload whose turns belong to
different sessions but share whole prompt chains. On a single-node vLLM run
(Qwen2.5-1.5B, 1,733 blocks, 16-token blocks) SIEVE beats LRU by 24.6% there,
and the trace-level sigma_cross is 0.995.

## Why it belongs in this repository

Eviction results are usually reported as a hit-ratio delta with no locality
descriptor, so two results that disagree cannot be reconciled without re-running
both traces. The statistic `sigma_cross`, capacity ratio `C/W`, pinned fraction and
the measured reuse-gap distribution describe the workload but **do not**
determine a cache-policy ranking without an actual simulation comparison.

```
cmake -G Ninja -B build && ninja -C build traceReuseStat
./_build/bin/traceReuseStat traces/freeinference.lcp.zst --gap-hist
```
