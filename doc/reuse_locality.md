# Reuse locality: the share of reuse that crosses sessions

`traceReuseStat` reports one number that decides whether an eviction policy can
matter on a given trace: **sigma_cross**, the fraction of *reuse* accesses whose
previous accessor belongs to a different session. Reuse that stays inside one
session is already captured by recency; reuse that crosses sessions only
survives if the victim choice protects a block whose last access is old.

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
both traces. `sigma_cross`, the capacity ratio `C / W` and the pinned fraction
are enough to place any result on the (sharing, slack) plane; this tool computes
the first one from a trace in one streaming pass, with the same reader the
simulator uses, so the number always matches what `prefixsim` replayed.

```
g++ -O2 -o traceReuseStat tools/traceReuseStat.cpp  # or: ninja -C _build traceReuseStat
./_build/bin/traceReuseStat traces/freeinference.lcp.zst --gap-hist
```
