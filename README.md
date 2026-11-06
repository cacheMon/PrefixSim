<p>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="doc/assets/logo-dark.svg">
    <img alt="PrefixSim" src="doc/assets/logo.svg" width="380">
  </picture>
</p>

[![build](https://github.com/cacheMon/PrefixSim/actions/workflows/build.yml/badge.svg)](https://github.com/cacheMon/PrefixSim/actions/workflows/build.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Dataset](https://img.shields.io/badge/%F0%9F%A4%97%20dataset-2026__prefixcache-yellow)](https://huggingface.co/datasets/harvardMadsys/2026_prefixcache)

**A fast simulator for the KV-block prefix cache of LLM serving engines.**

Engines such as vLLM and SGLang keep the KV cache of finished requests, so a new
request that shares a prefix with an earlier one skips recomputing it. When the
cache is full, an eviction policy decides which blocks to drop, and that choice
decides how much prefill compute the cache saves. `prefixsim` replays real LLM
request traces through such a cache and compares eviction policies by what
matters: the **compute saving ratio** (prefill compute avoided), not just the
block hit ratio.

On the freeinference production trace (see [Traces](#traces)), with a 16k-block cache (24 GiB of
Qwen3-Coder-30B's bf16 KV cache) and that model's measured cost per block:

| Policy | Block hit ratio | Compute saving ratio |
|---|---:|---:|
| `lru` | 0.6107 | 0.5333 |
| `s3fifo` | 0.6209 | 0.5491 |
| `random_compute` | 0.6068 | 0.6378 |
| `belady_compute` (offline oracle) | 0.6823 | 0.7353 |

The compute-aware `random_compute` saves 10 points more prefill compute than
LRU, at a lower hit ratio.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/figures/compute_saving-dark.svg">
  <img alt="Compute saving ratio against cache size, from 16k to 256k blocks, for LRU, S3-FIFO, RandomCompute and the BeladyCompute oracle, on the freeinference and chutes traces" src="doc/figures/compute_saving.svg">
</picture>

The gap is widest when the cache is tight. On freeinference, `random_compute`'s
lead over LRU shrinks from 10 points at 16k blocks to under 1 point from 64k
blocks on, where every policy saves over 91% of prefill compute. On chutes,
with 3.5 times as many distinct blocks, `random_compute` matches LRU from 32k
blocks on, S3-FIFO trails both, and the oracle stays ahead of LRU by 9 points
at 16k blocks and 2 points at 256k. The data and the script that plots it are
in [`doc/figures/`](doc/figures/).

## Why PrefixSim

- **The serving constraint, modelled.** A request needs *all* of its KV blocks
  resident at once, so each request is served as a unit: match, allocate,
  replay. Where the policy supports it, the blocks of the request being served
  are pinned while room is made, as an engine ref-counts them.
- **Compute-aware metrics.** Every block is weighted by the prefill compute a
  hit on it saves, under a cost model: `uniform`, `position`, the measured
  `qwen3coder30b_blksz_16`, `linear:A,B`, or a table of your own.
- **46 eviction policies**, from [libCacheSim](https://github.com/1a1a11a/libCacheSim):
  LRU, ARC, S3-FIFO, SIEVE, W-TinyLFU, LIRS, LHD and the other classics; the
  Belady and BeladyCompute oracles; published prefix-cache policies; and
  session-level and prefix-tree-aware policies. See
  [Eviction policies](#eviction-policies).
- **Streams real traces in one pass.** The [lcp](doc/lcp_format.md) format
  stores next-access times, so `prefixsim` holds one request at a time: an LRU
  run over 656M block accesses takes 105 s and 146 MiB (see
  [Speed and memory](#speed-and-memory)).
- **Production traces included.** Two traces from real LLM services are
  [on Hugging Face](https://huggingface.co/datasets/harvardMadsys/2026_prefixcache),
  already in lcp.

## Quick start

PrefixSim builds on libCacheSim, which comes in as a submodule, and needs
libCacheSim's dependencies (CMake, Ninja, glib and zstd, among others).

```bash
git clone --recursive https://github.com/cacheMon/PrefixSim.git
cd PrefixSim
bash libCacheSim/scripts/install_dependency.sh
cmake -G Ninja -B _build -DCMAKE_BUILD_TYPE=Release
ninja -C _build

# the two production traces, 308 MB in all (pip install huggingface_hub)
hf download harvardMadsys/2026_prefixcache --repo-type dataset --local-dir traces

# a 64k-block (96 GiB) cache under three policies, a few minutes
./_build/bin/prefixsim --trace traces/freeinference.lcp.zst --cache-size 64k \
    --algo lru,sieve,belady_compute --cost-model qwen3coder30b_blksz_16
```

`--cache-size` counts KV blocks (suffixes `k`, `m`, `g` are powers of 1024).
To simulate your own trace, convert it from JSONL, one request per line with a
`timestamp` and its per-block hashes, in prefix order, as `hash_ids`:

```bash
./_build/bin/traceConvLLM --input my_trace.jsonl --output my_trace.lcp
./_build/bin/prefixsim --trace my_trace.lcp.zst --cache-size 64k --algo lru,s3fifo
```

The [quickstart](doc/quickstart.md) walks through the options and the output.

## Eviction policies

The policies are implemented in libCacheSim. The name in `code` is what you
pass to `--algo`, and `prefixsim --list-algos` prints the ones your build has.
[Which algorithms work](doc/design.md#which-algorithms-work) explains how each
fits a prefix cache, and [pinning](doc/design.md#phase-2--allocate) which ones
keep a request's own blocks while making room for it.

Prefix-cache policies:

- Compute-aware, weighting each block by the prefill compute a hit on it saves
  (`--cost-model`):
  [BeladyCompute](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/BeladyCompute.c) `belady_compute` (needs future information),
  [GDSF-Compute](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/cpp/GDSF_compute.cpp) `gdsf_compute`,
  [LHD-Compute](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/LHD/LHD_compute_interface.cpp) `lhd_compute`,
  [RandomCompute](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/RandomCompute.c) `random_compute`,
  [RandomComputeSmallQueue](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/RandomComputeSmallQueue.c) `random_compute_small_queue`
- Published prefix-cache policies:
  [Workload-Aware](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/cpp/WorkloadAware.cpp) `workload_aware` ([ATC'25](https://arxiv.org/abs/2506.02634)),
  [AsymCache](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/cpp/AsymCache.cpp) `asym_cache` ([arXiv:2606.02964](https://arxiv.org/abs/2606.02964))
- Session-level, evicting a whole conversation rather than a block:
  [session policies](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/cpp/sessionCache.cpp) `session_lru`, `session_arc`,
  `session_s3fifo`, `session_random_compute`, `session_belady` (needs future
  information)
- Partial-node, sampling prefix-tree nodes and evicting a node's deepest block:
  [PartialNodeRandomCompute](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/cpp/PartialNodeRandomCompute.cpp) `partial_node_random_compute`,
  [PartialNodeRandomFreq](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/cpp/PartialNodeRandomFreq.cpp) `partial_node_random_freq`,
  [PartialNodeRandomComputeSmallQueue](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/cache/eviction/cpp/PartialNodeRandomComputeSmallQueue.cpp) `partial_node_random_compute_small_queue`

General-purpose policies, linked to their sources in
[libCacheSim's list](https://github.com/1a1a11a/libCacheSim#eviction-algorithms):

- `lru`, `fifo`, `clock`, `sieve`, `slru`, `mru`, `random`, `randomtwo`,
  `randomlru`, `lru-prob`, `size`, `fifo-merge`
- `lfu`, `lfuda`, `arc`, `twoq`, `car`, `lirs`, `hyperbolic`, `lecar`,
  `cacheus`, `lhd`, `gdsf`
- `wtinylfu`, `qdlp`, `s3fifo`, `s3fifod`
- `belady` (needs future information)
- `s4fifo`, `lrb` and `3lcache`, which need a build option:
  `-DENABLE_S4FIFO=ON`, `-DENABLE_LRB=ON` and `-DENABLE_3L_CACHE=ON`

## Traces

| File | Workload | Requests | Block accesses | Span |
|---|---|---:|---:|---:|
| `freeinference.lcp.zst` | coding and assistant agents on the FreeInference gateway | 327,548 | 656,240,179 | 7.0 days |
| `chutes.lcp.zst` | chat and roleplay on the Chutes inference proxy | 515,817 | 603,642,254 | 130.9 days |

Both are tokenized with the Qwen3-Coder-30B-A3B tokenizer and cut into 16-token
blocks, so 1k blocks hold 1.5 GiB of that model's bf16 KV cache. Block ids are
opaque integers; no text is released. The
[dataset card](https://huggingface.co/datasets/harvardMadsys/2026_prefixcache)
has the details.

## Speed and memory

One LRU run with a 64k-block cache, single-threaded on an AMD Threadripper PRO
7975WX, on the freeinference trace in each input format:

| Input | File size | Time | Peak memory |
|---|---:|---:|---:|
| JSONL | 6.79 GiB | 169 s | 12.9 GiB |
| `.lcp` | 9.80 GiB | 103 s | 80 MiB |
| `.lcp.zst` | 72.5 MiB | 105 s | 146 MiB |

The results are identical across formats. A JSONL trace has to be loaded whole
and annotated first; an lcp trace is streamed. Most of the `.lcp.zst` memory is
zstd's 128 MiB decompression window, which does not grow with the trace.

## Documentation

- [Quickstart](doc/quickstart.md): converting traces, running policies, reading
  the output.
- [Design](doc/design.md): how a request is served, why a block after a miss
  still counts, pinning, replay order, and which algorithms work.
- [lcp format](doc/lcp_format.md): the trace format specification.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | the simulator: trace readers, the serve loop, the lcp and JSONL writers, the CLI |
| `tools/` | `traceConvLLM`, the JSONL ↔ lcp converter |
| `test/` | tests of the serve loop and the lcp round trip |
| `doc/` | the documentation, the logo, and the README's figure with its data and plotting script |
| `libCacheSim/` | the [libCacheSim](https://github.com/1a1a11a/libCacheSim) submodule: the eviction algorithms and the lcp reader |

libCacheSim's build options pass through, for example `-DENABLE_S4FIFO=ON`, and
its other tools build by name: `ninja -C _build tracePrint cachesim`. A new
eviction policy goes into libCacheSim, and then into the table in
`src/simulator.cpp`.

## Citation

If you use PrefixSim or its traces, please cite:

```bibtex
@misc{liu2026fancyeviction,
  title         = {When Fancy Eviction Fails: Rethinking Cache Replacement For LLM Prefix Reuse},
  author        = {Liu, Yiyu and Yu, Minlan and Yang, Juncheng},
  year          = {2026},
  eprint        = {2609.28870},
  archivePrefix = {arXiv},
  primaryClass  = {cs.DC},
  url           = {https://arxiv.org/abs/2609.28870}
}
```

PrefixSim is built on libCacheSim; see its
[citation](https://github.com/1a1a11a/libCacheSim/blob/develop/CITATION.cff) too.

## License

PrefixSim is licensed under the [Apache License 2.0](LICENSE).
