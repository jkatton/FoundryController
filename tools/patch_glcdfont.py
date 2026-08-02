"""
PlatformIO pre-build script: overwrites two unused character slots in
TFT_eSPI's default GLCD font (Fonts/glcdfont.c) with custom button-icon
glyphs, so BUTTON_A_LABEL/BUTTON_B_LABEL (config.h) can print as small
icons instead of the letters "A"/"B" - through the exact same
tft.drawString()/String() calls Display.cpp already uses, no bitmap or
sprite drawing involved.

Runs on every `pio run` (see the `extra_scripts = pre:scripts/patch_glcdfont.py`
line this needs in platformio.ini), so nothing has to be hand-edited in
.pio/libdeps: PlatformIO can wipe/reinstall that folder at any time (a
clean checkout, `pio run -t clean`, a teammate's machine, CI, ...) and
this script just re-applies the patch before the next compile.

Why this is safe to repurpose: TFT_eSPI's classic 5x7 GLCD font (its
default font, font 1 - this project never calls setTextFont()) is a flat
256-entry table, 5 bytes per glyph, indexed directly by ASCII code.
Codes 0x60 (backtick) and 0x7E (tilde) are printable ASCII that this
project's on-screen text never actually prints, which is what makes them
safe/free to redefine. (Codes below 32 look tempting too, since they're
the old "control character" range, but TFT_eSPI's low-level glyph-drawing
routine unconditionally skips anything below 32 - `if (c < 32) return;`,
for every font - so a code in that range never actually paints; see
config.h's comment on BUTTON_A_LABEL/BUTTON_B_LABEL for the full story.)
"""

Import("env")

import os
import re
import glob

# ---------------------------------------------------------------------
# Custom glyphs
# ---------------------------------------------------------------------
# code -> 5 bytes, one per column (left to right). In each byte, bit 0 is
# the top pixel of that column and bit 6 is the bottom pixel (bits 7 and
# the font's implicit blank row below are unused - same layout as every
# other glyph already in the table).
#
# Codes 0x60 (backtick) / 0x7E (tilde) - NOT 1/2 or any other code below
# 32: TFT_eSPI's low-level glyph-drawing routine unconditionally does
# `if (c < 32) return;` before it ever looks at the font table, for every
# font, so anything in that range never actually paints (see config.h's
# comment on BUTTON_A_LABEL/BUTTON_B_LABEL for the full explanation).
#
# IMPORTANT: keep these two code numbers in sync with BUTTON_A_LABEL/
# BUTTON_B_LABEL in config.h - this script and config.h are two separate
# sources of truth for the same two numbers.
GLYPHS = {
    0x60: [0x22, 0x14, 0x08, 0x14, 0x22],  # BUTTON_A_LABEL: X (back quote)
    0x7E: [0x1C, 0x22, 0x22, 0x22, 0x1C],  # BUTTON_A_LABEL: O (tilde)
}

# Matches one glyph row: 5 comma-separated "0xNN" byte literals, with a
# trailing comma after the 5th byte on every row except the very last one
# in the array (ordinary C - a trailing comma on the final element is
# optional, and glcdfont.c's last row omits it), so that comma is
# optional here too. Any inline comment on that line (e.g. the "// #176
# ..." notes further down in the real file) sits just after this match
# and is left completely alone.
_GLYPH_RE = re.compile(
    r"0x[0-9A-Fa-f]{2}\s*,\s*0x[0-9A-Fa-f]{2}\s*,\s*0x[0-9A-Fa-f]{2}\s*,"
    r"\s*0x[0-9A-Fa-f]{2}\s*,\s*0x[0-9A-Fa-f]{2}\s*,?"
)


def _format_glyph(byte_values):
    return ", ".join("0x%02X" % b for b in byte_values) + ","


def _patch_text(text, glyphs):
    matches = list(_GLYPH_RE.finditer(text))
    if len(matches) < 256:
        # Not the font table we expect (wrong file, or the library
        # changed format) - bail out without touching anything rather
        # than risk corrupting an unrelated file.
        return text, False

    edits = []
    for code, byte_values in glyphs.items():
        if code < 0 or code >= len(matches):
            continue
        m = matches[code]
        new_str = _format_glyph(byte_values)
        if text[m.start():m.end()] != new_str:
            edits.append((m.start(), m.end(), new_str))

    if not edits:
        return text, False  # already patched - nothing to do

    edits.sort()
    pieces = []
    cursor = 0
    for start, end, new_str in edits:
        pieces.append(text[cursor:start])
        pieces.append(new_str)
        cursor = end
    pieces.append(text[cursor:])
    return "".join(pieces), True


def _find_font_files():
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    pioenv = env["PIOENV"]

    print("[patch_glcdfont] PROJECT_LIBDEPS_DIR = " + libdeps_dir)
    print("[patch_glcdfont] PIOENV = " + pioenv)

    direct = os.path.join(libdeps_dir, pioenv, "TFT_eSPI", "Fonts", "glcdfont.c")
    if os.path.isfile(direct):
        print("[patch_glcdfont] found " + direct)
        return [direct]

    # Fallback in case the installed folder isn't named exactly
    # "TFT_eSPI" (e.g. a pinned fork/version alias), or lives somewhere
    # unexpected under this env's libdeps.
    found = glob.glob(os.path.join(libdeps_dir, pioenv, "**", "glcdfont.c"), recursive=True)
    if found:
        print("[patch_glcdfont] found via fallback search: " + ", ".join(found))
        return found

    # Last resort: search every env's libdeps, plus a project-local lib/
    # folder if one exists - PlatformIO prefers a project-local lib/
    # over .pio/libdeps, so if TFT_eSPI has been vendored there, patching
    # only the libdeps copy would silently have no effect on the actual
    # build.
    print("[patch_glcdfont] not found under " + os.path.join(libdeps_dir, pioenv) +
          " - widening search to all envs and project-local lib/")
    found = glob.glob(os.path.join(libdeps_dir, "**", "glcdfont.c"), recursive=True)
    project_dir = env.subst("$PROJECT_DIR")
    found += glob.glob(os.path.join(project_dir, "lib", "**", "glcdfont.c"), recursive=True)
    if found:
        print("[patch_glcdfont] found via widened search: " + ", ".join(found))
    return found


def main():
    files = _find_font_files()
    if not files:
        print("[patch_glcdfont] WARNING: no glcdfont.c found anywhere - "
              "TFT_eSPI may not be installed yet (run `pio run` again after "
              "the first library fetch), or extra_scripts isn't wired up "
              "for this env in platformio.ini")
        return

    for path in files:
        with open(path, "r") as f:
            text = f.read()
        match_count = len(_GLYPH_RE.findall(text))
        print("[patch_glcdfont] %s: found %d glyph rows (need >= 256)" % (path, match_count))
        patched, changed = _patch_text(text, GLYPHS)
        if changed:
            with open(path, "w") as f:
                f.write(patched)
            print("[patch_glcdfont] patched " + path)
        else:
            print("[patch_glcdfont] " + path + " already up to date (or pattern didn't match - check the row count above)")


main()