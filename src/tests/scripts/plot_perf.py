#!/usr/bin/env python3
"""
plot_perf.py - Visualize perf_benchmark CSV results.

Usage:
    plot_perf.py <results.csv> [--output-dir=DIR]

Produces:
    1. Heatmaps: threads x tags, color = reads/sec (one per mode)
    2. Scaling plots: reads/sec vs threads, lines per tag count
    3. Efficiency plot: reads/sec vs CPU load
    4. Fairness overview

Requires: pip install matplotlib pandas seaborn
"""

import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import matplotlib.colors as mcolors
import seaborn as sns
import numpy as np


def load_csv(path):
    """Load CSV, skipping comment lines."""
    with open(path) as f:
        lines = [line for line in f if not line.startswith("#")]
    from io import StringIO
    df = pd.read_csv(StringIO("".join(lines)))
    return df


def plot_heatmaps(df, metric, label, fmt, outdir):
    """Heatmap per mode: rows=threads, cols=tags, color=metric."""
    modes = sorted(df["mode"].unique())

    all_threads = sorted(df["threads"].unique(), reverse=True)  # descending: 1000 at top
    all_tags    = sorted(df["tags"].unique())

    fig, axes = plt.subplots(
        len(modes), 1,
        figsize=(8, 3.5 * len(modes)),
        squeeze=False,
    )
    fig.suptitle(f"{label} by Configuration", fontsize=14, y=1.02)

    vmin = df[metric].min()
    vmax = df[metric].max()

    for row, mode in enumerate(modes):
        ax = axes[row][0]
        subset = df[df["mode"] == mode]

        if subset.empty:
            ax.set_visible(False)
            continue

        pivot = subset.pivot_table(
            index="threads", columns="tags", values=metric, aggfunc="mean"
        )
        # Reindex to full axis range; missing cells (threads > tags) become NaN.
        pivot = pivot.reindex(index=all_threads, columns=all_tags)

        # Build annotation array: show value or "N/A" for invalid combos.
        annot = pivot.copy().astype(object)
        for t in all_threads:
            for tag in all_tags:
                if pd.isna(pivot.loc[t, tag]):
                    annot.loc[t, tag] = "N/A"
                else:
                    val = pivot.loc[t, tag]
                    annot.loc[t, tag] = f"{val:{fmt}}"

        sns.heatmap(
            pivot, ax=ax, annot=annot, fmt="",
            cmap="YlOrRd" if "cpu" in metric else "YlGnBu",
            vmin=vmin, vmax=vmax,
            cbar=True,
            linewidths=0.5,
            mask=pivot.isna(),
        )
        # Shade invalid cells gray.
        pivot_nan = pivot.isna()
        if pivot_nan.any().any():
            sns.heatmap(
                pivot_nan.astype(float).where(pivot_nan, other=np.nan),
                ax=ax, annot=annot.where(pivot_nan, other=""), fmt="",
                cmap=mcolors.ListedColormap(["#cccccc"]),
                vmin=0, vmax=1, cbar=False, linewidths=0.5,
            )

        ax.set_title(mode, fontsize=10)
        ax.set_xlabel("tags" if row == len(modes) - 1 else "")
        ax.set_ylabel("threads")

    fig.tight_layout()
    fname = os.path.join(outdir, f"heatmap_{metric}.png")
    fig.savefig(fname, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  {fname}")


def plot_scaling(df, outdir):
    """Line plots: reads/sec vs threads, one line per tag count, one subplot per mode."""
    modes = sorted(df["mode"].unique())

    fig, axes = plt.subplots(
        1, len(modes),
        figsize=(5 * len(modes), 4),
        squeeze=False, sharex=True,
    )
    fig.suptitle("Throughput Scaling: Reads/sec vs Threads", fontsize=14, y=1.02)

    palette = sns.color_palette("tab10", n_colors=df["tags"].nunique())
    tag_vals = sorted(df["tags"].unique())

    for col, mode in enumerate(modes):
        ax = axes[0][col]
        subset = df[df["mode"] == mode]

        for i, t in enumerate(tag_vals):
            data = subset[subset["tags"] == t].sort_values("threads")
            if data.empty:
                continue
            ax.plot(
                data["threads"], data["reads_per_sec"],
                marker="o", markersize=4, label=f"{t} tags",
                color=palette[i], linewidth=1.5,
            )

        ax.set_title(mode, fontsize=10)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
        ax.yaxis.set_major_formatter(ticker.EngFormatter())
        ax.set_xlabel("threads")
        if col == 0:
            ax.set_ylabel("reads/sec")
        if col == len(modes) - 1:
            ax.legend(fontsize=7, loc="best")

    fig.tight_layout()
    fname = os.path.join(outdir, "scaling_reads_per_sec.png")
    fig.savefig(fname, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  {fname}")


def plot_efficiency(df, outdir):
    """Scatter: reads/sec vs CPU load, color=mode, size=threads."""
    fig, ax = plt.subplots(figsize=(8, 5))

    palette = {"sync": "steelblue", "async": "darkorange"}
    for mode, marker in [("sync", "o"), ("async", "^")]:
        subset = df[df["mode"] == mode]
        if subset.empty:
            continue
        ax.scatter(
            subset["cpu_load_pct"], subset["reads_per_sec"],
            s=subset["threads"] / subset["threads"].max() * 200 + 20,
            alpha=0.6, marker=marker, label=mode,
            color=palette[mode],
            edgecolors="k", linewidths=0.3,
        )

    ax.set_xlabel("CPU Load (%)")
    ax.set_ylabel("Reads/sec")
    ax.set_title("Throughput vs CPU Efficiency")
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(ticker.EngFormatter())
    ax.legend()

    fig.tight_layout()
    fname = os.path.join(outdir, "efficiency.png")
    fig.savefig(fname, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  {fname}")


def plot_fairness(df, outdir):
    """Fairness CV heatmaps, one per mode."""
    modes = sorted(df["mode"].unique())

    all_threads = sorted(df["threads"].unique(), reverse=True)
    all_tags    = sorted(df["tags"].unique())

    fig, axes = plt.subplots(
        len(modes), 1,
        figsize=(8, 3.5 * len(modes)),
        squeeze=False,
    )
    fig.suptitle("Fairness (CV %): lower = more fair", fontsize=14, y=1.02)

    vmax = min(df["fairness_cv"].quantile(0.95), 100)

    for row, mode in enumerate(modes):
        ax = axes[row][0]
        subset = df[df["mode"] == mode]

        if subset.empty:
            ax.set_visible(False)
            continue

        pivot = subset.pivot_table(
            index="threads", columns="tags", values="fairness_cv", aggfunc="mean"
        )
        pivot = pivot.reindex(index=all_threads, columns=all_tags)

        annot = pivot.copy().astype(object)
        for t in all_threads:
            for tag in all_tags:
                if pd.isna(pivot.loc[t, tag]):
                    annot.loc[t, tag] = "N/A"
                else:
                    annot.loc[t, tag] = f"{pivot.loc[t, tag]:.0f}"

        sns.heatmap(
            pivot, ax=ax, annot=annot, fmt="",
            cmap="RdYlGn_r", vmin=0, vmax=vmax,
            cbar=True,
            linewidths=0.5,
            mask=pivot.isna(),
        )
        if pivot.isna().any().any():
            sns.heatmap(
                pivot.isna().astype(float).where(pivot.isna(), other=np.nan),
                ax=ax, annot=annot.where(pivot.isna(), other=""), fmt="",
                cmap=mcolors.ListedColormap(["#cccccc"]),
                vmin=0, vmax=1, cbar=False, linewidths=0.5,
            )

        ax.set_title(mode, fontsize=10)
        ax.set_xlabel("tags" if row == len(modes) - 1 else "")
        ax.set_ylabel("threads")

    fig.tight_layout()
    fname = os.path.join(outdir, "fairness_cv.png")
    fig.savefig(fname, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  {fname}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--")}

    if not args:
        print(f"Usage: {sys.argv[0]} <results.csv> [--output-dir=DIR]")
        sys.exit(2)

    csv_path = args[0]
    outdir = flags.get("--output-dir", os.path.dirname(csv_path) or ".")
    os.makedirs(outdir, exist_ok=True)

    df = load_csv(csv_path)
    print(f"Loaded {len(df)} rows from {csv_path}")
    print(f"Output directory: {outdir}\n")

    print("Generating charts:")
    plot_heatmaps(df, "reads_per_sec", "Reads/sec", ".0f", outdir)
    plot_heatmaps(df, "cpu_load_pct", "CPU Load (%)", ".1f", outdir)
    plot_scaling(df, outdir)
    plot_efficiency(df, outdir)
    plot_fairness(df, outdir)

    print(f"\nDone. 5 charts in {outdir}/")


if __name__ == "__main__":
    main()
