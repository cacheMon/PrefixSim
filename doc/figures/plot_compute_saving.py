#!/usr/bin/env python3
"""Plot compute saving ratio against cache size, for the README.

Reads the RESULT lines of prefixsim runs (compute_saving.txt, next to this
script) and writes compute_saving.svg and compute_saving-dark.svg, one for
each GitHub theme. Requires matplotlib; uses Inter when it is installed.

To regenerate the data: 40 runs, with the traces from Hugging Face in
traces/. They take a few hours on one core, since every run streams a whole
trace, so run them in parallel if you can. --no-verify skips a self-check
after each request, which saves time and does not change the results.

    for t in freeinference chutes; do
      for c in 16k 32k 64k 128k 256k; do
        for a in lru s3fifo random_compute belady_compute; do
          ./_build/bin/prefixsim --trace traces/$t.lcp.zst --cache-size $c \\
              --algo $a --cost-model qwen3coder30b_blksz_16 --no-verify
        done
      done
    done | grep '^RESULT' | sed 's|trace=[^ ]*/|trace=|' > doc/figures/compute_saving.txt

    python3 doc/figures/plot_compute_saving.py
"""
import os
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib import font_manager  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# 16-token blocks of Qwen3-Coder-30B's bf16 KV cache: 1k blocks are 1.5 GiB.
GIB_PER_BLOCK = 1.5 / 1024

TRACES = [
    ("freeinference.lcp.zst", "freeinference", "coding and assistant agents, 7 days"),
    ("chutes.lcp.zst", "chutes", "chat and roleplay, 131 days"),
]

# (algo, label, light color, dark color, marker, linestyle). Categorical slots
# 1-3 of a palette checked for colour-vision deficiency; the offline oracle is
# drawn in neutral ink, dashed, as a bound rather than a competitor.
SERIES = [
    ("lru", "LRU", "#2a78d6", "#3987e5", "o", "-"),
    ("s3fifo", "S3-FIFO", "#eb6834", "#d95926", "s", "-"),
    ("random_compute", "RandomCompute", "#1baf7a", "#199e70", "D", "-"),
    ("belady_compute", "BeladyCompute (offline oracle)", "#52514e", "#c3c2b7", "^", "--"),
]

THEMES = {
    "light": dict(ink="#0b0b0b", ink2="#52514e", muted="#6e6d68", grid="#e1e0d9",
                  axis="#c3c2b7", surface="#ffffff"),
    "dark": dict(ink="#f0efec", ink2="#c3c2b7", muted="#9a988f", grid="#2c2c2a",
                 axis="#4a4a46", surface="#0d1117"),
}


def load(path):
    data = defaultdict(dict)  # (trace, algo) -> {cache_size: ratio}
    with open(path) as f:
        for line in f:
            if not line.startswith("RESULT"):
                continue
            kv = dict(tok.split("=", 1) for tok in line.split()[1:])
            trace = os.path.basename(kv["trace"])
            data[(trace, kv["algo"])][int(kv["cache_size"])] = float(kv["compute_saving_ratio"])
    return data


def size_label(blocks):
    return f"{blocks // 1024}k\n{blocks * GIB_PER_BLOCK:g} GiB"


def setup_font():
    for name in ("Inter", "Inter Display"):
        for f in font_manager.findSystemFonts():
            if os.path.basename(f).startswith(name.replace(" ", "") + "-"):
                font_manager.fontManager.addfont(f)
    names = {f.name for f in font_manager.fontManager.ttflist}
    plt.rcParams["font.family"] = "Inter" if "Inter" in names else "DejaVu Sans"


def plot(data, theme, out):
    t = THEMES[theme]
    plt.rcParams.update({
        "svg.fonttype": "path",  # glyphs as outlines: renders without the font
        "svg.hashsalt": "prefixsim",  # stable ids, so regenerating gives the same file
        "font.size": 11,
        "text.color": t["ink"],
        "axes.labelcolor": t["ink2"],
        "xtick.color": t["muted"],
        "ytick.color": t["muted"],
        "xtick.labelcolor": t["ink2"],
        "ytick.labelcolor": t["ink2"],
    })
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.1), sharey=True, layout="constrained")
    fig.get_layout_engine().set(w_pad=0.1, h_pad=0.08, wspace=0.06)
    fig.patch.set_alpha(0)

    ys = [v for d in data.values() for v in d.values()]
    lo = max(0.0, (int(min(ys) * 10) / 10))
    hi = min(1.0, (int(max(ys) * 10) + 1) / 10)

    for ax, (trace, name, desc) in zip(axes, TRACES):
        ax.set_facecolor("none")
        for algo, label, light, dark, marker, ls in SERIES:
            pts = sorted(data.get((trace, algo), {}).items())
            if not pts:
                continue
            xs, vs = zip(*pts)
            color = light if theme == "light" else dark
            ax.plot(xs, vs, ls, color=color, lw=2, marker=marker, ms=7,
                    mec=t["surface"], mew=1.5, label=label, solid_capstyle="round",
                    dash_capstyle="round", zorder=3)
        xs = sorted({x for (tr, _), d in data.items() if tr == trace for x in d})
        if not xs:
            continue
        ax.set_xscale("log", base=2)
        ax.set_xticks(xs, [size_label(x) for x in xs])
        ax.minorticks_off()
        ax.set_xlim(xs[0] / 1.15, xs[-1] * 1.15)
        ax.set_ylim(lo, hi)
        ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:.0%}"))
        ax.grid(axis="y", color=t["grid"], lw=0.8, zorder=0)
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.spines["bottom"].set_color(t["axis"])
        ax.tick_params(axis="both", length=0, pad=6)
        ax.set_title(f"{name}", loc="left", fontsize=13, fontweight="semibold",
                     color=t["ink"], pad=22)
        ax.text(0, 1.035, desc, transform=ax.transAxes, fontsize=10.5, color=t["ink2"])
    axes[0].set_ylabel("compute saving ratio", fontsize=10.5)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside upper center", ncol=len(labels), frameon=False,
               fontsize=10.5, handlelength=2.4, columnspacing=1.8, labelcolor=t["ink2"])
    fig.supxlabel("cache size, in KV blocks and in GiB of Qwen3-Coder-30B's bf16 KV cache",
                  fontsize=10, color=t["ink2"])
    fig.savefig(out, format="svg", transparent=True, metadata={"Date": None})
    plt.close(fig)


def main():
    setup_font()
    data = load(os.path.join(HERE, "compute_saving.txt"))
    plot(data, "light", os.path.join(HERE, "compute_saving.svg"))
    plot(data, "dark", os.path.join(HERE, "compute_saving-dark.svg"))


if __name__ == "__main__":
    main()
