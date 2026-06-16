#!/usr/bin/env python3
"""Batch-render every full-screen Bumpy .VEC image to PNG via the standalone, pure-
Python decoder in vec_to_png.py (op4 -> vec_run -> op12 -> planar@99 + embedded
palette@51). World maps go to local/results/levels_png/, other screens to local/results/images/.

Usage: render_vec_images.py            # render the default set
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/extract"))
from vec_to_png import decode_vec_to_png  # noqa: E402

GAME = os.path.join(ROOT, "local/build/capture/game")
# (vec filename, output dir) — full-screen image .VECs (op4/op12 record streams)
TARGETS = ([("MONDE%d.VEC" % n, "local/results/levels_png", "world%d.png" % n) for n in range(1, 10)]
           + [("TITRE.VEC", "local/results/images", "titre.png"),
              ("DESSFIN.VEC", "local/results/images", "dessfin.png"),
              ("SCORE.VEC", "local/results/images", "score.png"),
              ("BUMPRESE.VEC", "local/results/images", "bumprese.png"),   # "BUMpy PRESEntation" splash
              ("MASKBUMP.VEC", "local/results/images", "maskbump.png")])  # EASY/MEDIUM/HARD difficulty UI


def main() -> None:
    for vec, outdir, name in TARGETS:
        src = os.path.join(GAME, vec)
        if not os.path.exists(src):
            print("skip %s (missing)" % vec)
            continue
        dst = os.path.join(ROOT, outdir, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        decode_vec_to_png(src, dst)
        print("%-14s -> %s" % (vec, os.path.relpath(dst, ROOT)), flush=True)


if __name__ == "__main__":
    main()
