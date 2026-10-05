# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

"""The lib/vterm boundary: the VT core includes only its own headers,
plt, libstd and system headers. Any mention of lib/shitty - by full
path or by a quoted include that only resolves there - is a violation.
The whole point of the split is that the core cannot see the GUI, the
fonts or the renderers; this check is the guarantee."""

import re
import sys
from pathlib import Path


# Headers generated into the build directory; a quoted include cannot
# resolve inside lib/vterm, and they are ours, not lib/shitty's.
GENERATED = {
    "parser.rl.h",
    "input_keys.h",
    "unicode_data.h",
}

INCLUDE = re.compile(r'^\s*#\s*include\s+(<[^>]+>|"[^"]+")')


# The crossings the boundary carries today, and only those. Until G15 this
# check was all or nothing, so the six crossings it then held - which no task
# in wave 6 was allowed to close - held the whole node red. The node is inside
# add_test(), so `./build test` stopped here and ran no test at all: Tests
# Alpine and Tests Fedora were blind from eb016aec (M6b) through four merges,
# on run 33862433804 and the three before it. A guard that cannot be told
# "this one, this many, and here is why" is a guard that gets deleted or,
# worse, left red until nobody reads it.
#
# Empty, and that is the whole of the news. It held six crossings until the
# window's insets, the in-band resize and the pane list's cell count started
# reaching the core through VtHost (contentInsets, surfaceResized,
# cellCapacityExcept): vterm.cpp lost composer.h, session.h and
# grid_geometry.h with them, and vt_headless.cpp - an embedder's adapter that
# had been standing on the lib/shitty side of the boundary while living in
# lib/vterm - moved to lib/shitty, which is where its other three went.
#
# Kept as an empty dict rather than deleted, because the shape is the
# guarantee and the next crossing will want to be written down here rather
# than argued about. What made it an allowance and not a pardon still holds
# and is worth keeping written:
#
# 1. It is keyed by file *and* by the include, not by a count per file. The
#    four guards in build.py can only key on a path, because what they meter
#    is a call and a call has no name of its own; G1 wrote that weakness down
#    (invariants 3.4, point 1): "composer.cpp: 8" means any eight, so removing
#    a legitimate call and adding an illegitimate one keeps the count and the
#    green. Here the subject *does* have a name - the include - so the key
#    carries it, and substitution is caught, not just growth.
#
# 2. The number is a count, and it meters the file it names. A second
#    #include of an already-allowed header is one more than the key says, and
#    one more is red. Duplicate includes are how a header arrives twice under
#    two spellings and how a crossing survives the file that legitimised it.
#
# 3. Every key says what would close it. A key with no way out is a key that
#    outlives its reason, and the next reader cannot tell an allowance from a
#    surrender.
#
# What is deliberately *not* red: an include listed here with no hits left. A
# count may fall to zero with the key kept, which is what T5.8 did to
# mouse_geometry_allowance - zero is the tightest number, and dropping the key
# with it would also drop the stale-file check that proves the guard can still
# see where the form would come back. The stale check below is over files, so
# a zeroed include keeps its file reachable and its guarantee alive.
ALLOWANCE = {}


# What the emptiness above cost, and what the three checks below buy back.
#
# The orphaned-key check was, as a side effect, this guard's proof of life: a
# key naming a file the scan never read went red, so a green meant the scan
# had reached the tree. Task A closed all six crossings, ALLOWANCE went empty,
# and the proof went with it - silently, because a green stayed a green.
# Measured by T7.1: `check_includes.py /tmp/empty /tmp/x.stamp` answered
# EXIT=0. A guard that reports success over nothing at all is not a criterion.
#
# Three things now have to hold before the stamp is written, and none of them
# is a number anybody has to maintain:
#
# 1. The root handed in is the directory this file lives in. The guard lives
#    inside the tree it guards, so this is true by construction and stays true
#    across every rename, addition and deletion in lib/vterm. It is what makes
#    an empty directory, a missing one, or a build.py pointing somewhere stale
#    red rather than green. A file count would not: any threshold over the
#    ~81 sources here is a number that gets nudged instead of read.
#
# 2. The scan witnessed the shapes a C++ core cannot be without: a header, a
#    source file, an angled include, and a quoted include that resolved inside
#    the core. These are properties of the tree's construction, not of its
#    size, and they are the tighter half of the guarantee: the root can be
#    right while the glob, the read or the regex quietly stops returning
#    anything, and then every arm below runs over nothing and finds nothing.
#    The witness is collected by the scan itself, from what it actually
#    matched, so it cannot be satisfied by a scan that did not happen.
#
# 3. The classifier still classifies. With the boundary closed and ALLOWANCE
#    empty, *no file in lib/vterm exercises a single violation arm* - the
#    guard's entire reason for existing is dead code against the real tree,
#    and deleting any of it leaves the build green. So the arms are exercised
#    against a sample carried in this file, whose expected output is written
#    out in full: the two spellings of a lib/shitty crossing, an include that
#    resolves nowhere, a reach outside the core, the allowance being spent
#    once and reporting the repeat, and the orphaned key. This is F7's premise
#    inside the test, applied to a guard: it asserts it can still see before
#    it reports that there is nothing to see.
#
# All three live here rather than in build.py on purpose, for the reason G15
# kept the allowance here: the node already holds this script among its
# inputs, so it rebuilds when this changes, and no second place exists where
# the guarantee could drift away from the guard.
WITNESSES = (
    "a header",
    "a source file",
    "an angled include",
    "a quoted include resolving inside the core",
)


def scan(sources, local, allowance):
    """Classify the includes of `sources`, a sequence of (name, text).

    Returns the violations, the allowance keys naming files that were not
    among the sources, and the witness set - the shapes actually met.
    """
    seen = set()
    counted = {}
    violations = []
    witness = set()
    for name, text in sources:
        seen.add(name)
        if name.endswith(".h"):
            witness.add("a header")
        elif name.endswith(".cpp"):
            witness.add("a source file")
        allowed = allowance.get(name, {})
        for number, line in enumerate(text.splitlines(), 1):
            match = INCLUDE.match(line)
            if match is None:
                continue
            spec = match.group(1)
            include = spec[1:-1]
            where = f"{name}:{number}"
            if "lib/shitty" in include:
                complaint = f"{where}: {spec} crosses into lib/shitty"
            elif spec.startswith('"'):
                if include in local or include in GENERATED:
                    witness.add("a quoted include resolving inside the core")
                    continue
                complaint = f"{where}: {spec} does not resolve inside lib/vterm"
            elif include.startswith("lib/") and not include.startswith("lib/vterm/"):
                complaint = f"{where}: {spec} reaches outside the core"
            else:
                witness.add("an angled include")
                continue
            # The count is spent in file order, so the allowed crossing is the
            # first one and every repeat of it is reported.
            counted[name, include] = counted.get((name, include), 0) + 1
            if counted[name, include] > allowed.get(include, 0):
                violations.append(complaint)
    return violations, sorted(set(allowance) - seen), witness


def check(root):
    sources = sorted(
        path
        for pattern in ("*.h", "*.cpp")
        for path in root.glob(pattern)
    )
    local = {path.name for path in sources}
    return scan(
        [(path.name, path.read_text()) for path in sources],
        local,
        ALLOWANCE,
    )


# One file of each kind, carrying one of every shape the classifier knows,
# and the answer it must give. Written out rather than derived, because a
# sample whose expectation is computed by the code under test agrees with
# whatever that code has become.
SELF_TEST_SOURCES = (
    ("core.h", '#include <stddef.h>\n#include "peer.h"\n'),
    ("core.cpp", "\n".join((
        '#include "core.h"',                    # 1 local, fine
        '#include "lib/shitty/composer.h"',     # 2 crossing, quoted
        '#include <lib/shitty/session.h>',      # 3 crossing, angled
        '#include "nowhere.h"',                 # 4 resolves nowhere, allowed
        '#include "nowhere.h"',                 # 5 the repeat, one too many
        '#include "parser.rl.h"',               # 6 generated, fine
        '#include <lib/plt/window.h>',          # 7 outside the core
        '#include <lib/vterm/screen.h>',        # 8 our own, fine
        '  #  include\t<std/str/view.h>',       # 9 spacing the regex must eat
        'x #include "lib/shitty/session.h"',    # 10 not an include at all
    )) + "\n"),
)
SELF_TEST_LOCAL = {"core.h", "core.cpp", "peer.h"}
SELF_TEST_ALLOWANCE = {
    "core.cpp": {"nowhere.h": 1},
    "gone.cpp": {"vanished.h": 1},
}
SELF_TEST_VIOLATIONS = [
    'core.cpp:2: "lib/shitty/composer.h" crosses into lib/shitty',
    "core.cpp:3: <lib/shitty/session.h> crosses into lib/shitty",
    'core.cpp:5: "nowhere.h" does not resolve inside lib/vterm',
    "core.cpp:7: <lib/plt/window.h> reaches outside the core",
]
SELF_TEST_STALE = ["gone.cpp"]


def self_test():
    failures = []
    violations, stale, witness = scan(
        SELF_TEST_SOURCES, SELF_TEST_LOCAL, SELF_TEST_ALLOWANCE
    )
    if violations != SELF_TEST_VIOLATIONS:
        failures.append(f"violations: {violations} != {SELF_TEST_VIOLATIONS}")
    if stale != SELF_TEST_STALE:
        failures.append(f"orphaned keys: {stale} != {SELF_TEST_STALE}")
    if sorted(witness) != sorted(WITNESSES):
        failures.append(f"witness: {sorted(witness)} != {sorted(WITNESSES)}")
    # And the other half of the witness: it has to come from the scan and not
    # from the scan having been written. Nothing read, nothing witnessed.
    _, _, empty = scan((), set(), {})
    if empty:
        failures.append(f"witness over no sources: {sorted(empty)} != []")
    return failures


def main():
    root = Path(sys.argv[1])
    stamp = Path(sys.argv[2])
    failures = self_test()
    if failures:
        print(
            "this check no longer answers correctly about includes it made "
            "up itself, so nothing it says about lib/vterm means anything:",
            file=sys.stderr,
        )
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    here = Path(__file__).parent
    if root.resolve() != here.resolve():
        print(
            f"this check guards the directory it lives in, and was pointed "
            f"at {root} instead of {here}: a guard reading somewhere else "
            f"reports on somewhere else, and over an empty or missing "
            f"directory it reports success.",
            file=sys.stderr,
        )
        return 1
    violations, stale, witness = check(root)
    if violations:
        print(
            "the VT core cannot see the GUI: these crossings are not in the "
            "allowance at the top of this file, and closing one is cheaper "
            "than widening it.",
            file=sys.stderr,
        )
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1
    if stale:
        # The trap T2.1 built for the four guards in build.py, and the reason
        # it exists: a key naming a file the scan no longer reaches is a
        # pardon nothing can spend, so the guard would go green over a file it
        # has stopped reading. It caught an orphaned key on M6b and went red
        # from both sides at once on M6e.
        print(
            "the boundary allowance names files this check never read, so it "
            "is guarding a core that no longer exists: re-key the allowance "
            "onto where these live now, or drop them.",
            file=sys.stderr,
        )
        for name in stale:
            print(f"  {name}", file=sys.stderr)
        return 1
    missing = [shape for shape in WITNESSES if shape not in witness]
    if missing:
        print(
            f"this check read {root} and never met the shapes a VT core "
            f"cannot be without, so its silence is about nothing: the glob, "
            f"the read or the include pattern stopped returning anything.",
            file=sys.stderr,
        )
        for shape in missing:
            print(f"  never saw {shape}", file=sys.stderr)
        return 1
    stamp.write_text("ok\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
