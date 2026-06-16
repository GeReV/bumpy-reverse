#!/usr/bin/env python3
"""Drive dosbox-staging to boot Bumpy's Arcade Fantasy and capture a screen
(default: the title, TITRE.VEC) as a ground-truth reference image for the pure-
Python .VEC decoder.

Why this exists: the .VEC renderer (op4 handler) is a large, state-dependent DOS
routine. Rather than fully emulate it, we run the *real* game once under DOSBox
and screenshot what it draws, giving a reference to validate the decoder against.

Pipeline (all local, no apt):
  1. Copy the game files to a writable work dir (keeps originals/ pristine).
  2. Generate a minimal dosbox-staging .conf (mount + autoexec BUMPY.EXE).
  3. Launch dosbox-staging on the current X display (WSLg :0), HOME redirected so
     it can write its config in the sandbox.
  4. Find the DOSBox window (python-xlib), focus it, and inject the startup menu
     keystrokes on a timer (video mode, sound) so the title renders.
  5. Grab the window with ImageMagick `import`, then downscale to the DOS 320x200.
  6. Tear everything down.

Dependencies: the bundled tools/dosbox-staging-* binary, python-xlib (pip), and
ImageMagick `import`/`convert`. See tools/capture/README.md.

Usage:
  tools/venv-emu/bin/python tools/capture/capture_title.py \
      [--keys F3,F7] [--boot-wait 4] [--title-wait 5] [--display :0] \
      [--out build/capture/title.png]
"""
import os, sys, time, subprocess, shutil, argparse

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DBX = os.path.join(ROOT, "tools/dosbox-staging-linux-x86_64-0.82.2-5e2ba/dosbox")
GAME_SRC = os.path.join(ROOT, "local/originals/old-games/bumpy")
WORK = os.path.join(ROOT, "local/build/capture")


def log(*a):
    print("[capture]", *a, flush=True)


def make_workdir():
    game = os.path.join(WORK, "game")
    home = os.path.join(WORK, "home")
    if os.path.isdir(game):
        shutil.rmtree(game, ignore_errors=True)
    for d in (game, home, os.path.join(home, ".config")):
        os.makedirs(d, exist_ok=True)
    # copy game files fresh and force-writable (some originals are read-only, and
    # DOSBox needs to write QUELDISK etc.) — keeps originals/ pristine.
    for f in os.listdir(GAME_SRC):
        src = os.path.join(GAME_SRC, f)
        if os.path.isfile(src):
            dst = os.path.join(game, f)
            shutil.copyfile(src, dst)
            os.chmod(dst, 0o644)
    conf = os.path.join(WORK, "bumpy.conf")
    with open(conf, "w") as fh:
        fh.write(
            "[sdl]\n"
            "fullscreen = false\n"
            "[render]\n"
            "glshader = sharp\n"          # pixel-perfect nearest scaling, no crt-auto CRT filter
            "aspect = false\n"            # square 1:1 pixels (raw 320x200 grid)
            "[autoexec]\n"
            "mount c %s\n"
            "c:\n"
            "BUMPY.EXE\n" % game)
    return game, home, conf


def _matches(w):
    # Match the DOSBox window robustly: its WM_NAME changes to the running
    # program (e.g. "BUMPY.EXE - 3000 cycles/ms"), so also match the stable
    # "cycles/ms" suffix and the WM_CLASS.
    try:
        name = (w.get_wm_name() or "").lower()
    except Exception:
        name = ""
    # NB: do NOT match bare "bumpy" — the Ghidra project window is "BumpyDecomp".
    # "cycles/ms" is unique to the DOSBox title bar.
    if "dosbox" in name or "cycles/ms" in name:
        return True
    try:
        cls = w.get_wm_class()
    except Exception:
        cls = None
    if cls and any("dosbox" in (c or "").lower() for c in cls):
        return True
    return False


def find_dosbox_window(disp, timeout=20):
    root = disp.screen().root
    end = time.time() + timeout
    while time.time() < end:
        def scan(w, depth=0):
            try:
                kids = w.query_tree().children
            except Exception:
                return None
            for c in kids:
                if _matches(c):
                    return c
                if depth < 2:
                    r = scan(c, depth + 1)
                    if r:
                        return r
            return None
        hit = scan(root)
        if hit:
            return hit
        time.sleep(0.4)
    return None


def focus_window(disp, win):
    # Raise + focus the window, then click inside it (absolute root coords) so the
    # compositor gives it keyboard focus — XTEST keys go to the focused window.
    from Xlib import X
    from Xlib.ext import xtest
    try:
        win.configure(stack_mode=X.Above)
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
    except Exception:
        pass
    geom = win.get_geometry()
    abspos = win.translate_coords(disp.screen().root, 0, 0)
    cx = -abspos.x + geom.width // 2
    cy = -abspos.y + geom.height // 2
    xtest.fake_input(disp, X.MotionNotify, x=cx, y=cy); disp.sync(); time.sleep(0.2)
    xtest.fake_input(disp, X.ButtonPress, 1); disp.sync(); time.sleep(0.05)
    xtest.fake_input(disp, X.ButtonRelease, 1); disp.sync(); time.sleep(0.4)


def press_key(disp, k):
    from Xlib import X, XK
    from Xlib.ext import xtest
    sym = getattr(XK, "XK_" + k, None)
    if sym is None:
        log("  unknown key:", k); return
    code = disp.keysym_to_keycode(sym)
    log("  key %s" % k)
    xtest.fake_input(disp, X.KeyPress, code); disp.sync(); time.sleep(0.05)
    xtest.fake_input(disp, X.KeyRelease, code); disp.sync(); time.sleep(0.6)


def grab(win_id, out_png):
    raw = out_png + ".raw.png"
    subprocess.run(["import", "-window", str(win_id), raw], check=True,
                   env={**os.environ})
    # The DOSBox window letterboxes the 320x200 image in black bars; trim those,
    # then nearest-downscale the content to the native 320x200 grid (no blur).
    subprocess.run(["convert", raw, "-bordercolor", "black", "-fuzz", "6%",
                    "-trim", "+repage", "-filter", "point",
                    "-resize", "320x200!", out_png], check=True)
    return raw


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    # Timeline tokens (space-separated): wN = wait N seconds, kKEY = press KEY,
    # sNAME = screenshot to build/capture/NAME.png. Default boots to the title.
    ap.add_argument("--timeline", default="w7 kF3 kF7 w6 stitle",
                    help="space-separated steps: wN (wait), kKEY (press), sNAME (shot)")
    ap.add_argument("--display", default=os.environ.get("DISPLAY", ":0"))
    args = ap.parse_args()

    if not os.path.exists(DBX):
        sys.exit("dosbox-staging binary not found: " + DBX)
    game, home, conf = make_workdir()
    log("workdir:", WORK)

    env = dict(os.environ)
    env["DISPLAY"] = args.display
    env["HOME"] = home              # redirect config writes into the sandbox
    env["XDG_CONFIG_HOME"] = os.path.join(home, ".config")
    env["SDL_VIDEODRIVER"] = "x11"

    log("launching dosbox-staging on", args.display)
    proc = subprocess.Popen([DBX, "-conf", conf], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        from Xlib import display as xdisplay
        disp = xdisplay.Display(args.display)
        win = find_dosbox_window(disp)
        if win is None:
            raise RuntimeError("DOSBox window not found on " + args.display)
        log("found window:", win.get_wm_name(), "id", hex(win.id))
        focused = False
        for tok in args.timeline.split():
            kind, val = tok[0], tok[1:]
            if kind == "w":
                log("wait %ss" % val); time.sleep(float(val))
            elif kind == "k":
                if not focused:
                    focus_window(disp, win); focused = True
                press_key(disp, val)
            elif kind == "s":
                out = os.path.join(WORK, val + ".png")
                log("shot -> %s" % out); grab(win.id, out)
            else:
                log("unknown timeline token:", tok)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        log("dosbox terminated")


if __name__ == "__main__":
    main()
