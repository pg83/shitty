# Copyright (C) 2026 Shitty team
# MIT licensed
# See the file LICENSE.MIT for the full license.

import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from harness import PRETTY, ROOT, SHITTY, Shitty, run_startup_failure


def print_config(binary):
    """-printConfig, with the developer's real ~/.config kept out.

    The same way harness.py keeps it out of every other run: the option
    reads the config before it prints, and a key that config holds which
    the binary does not know lands on stderr as a warning - which is
    exactly the stderr the tests below assert is empty. Measured on a
    machine whose ~/.config/shitty/shitty.toml carried such a key.
    """
    environment = os.environ.copy()
    environment["XDG_CONFIG_HOME"] = "/nonexistent"
    return subprocess.run(
        [str(binary), "-printConfig"], capture_output=True, env=environment
    )


EXAMPLE_CONFIG = ROOT / "bin" / "st" / "shitty.toml"
PRETTY_EXAMPLE_CONFIG = ROOT / "bin" / "pt" / "pretty.toml"


# The two example configs are the same file under two brands, and the
# only licensed difference between them is the brand name itself. Every
# value that differs has to differ by exactly this substitution; a value
# that differs any other way is the mine this check exists to catch.
#
# A rule rather than a list of excused keys, deliberately. A list would
# have to be extended by hand for every brand-specific option added
# after it, and the extension is the step that gets forgotten - which is
# the very failure the check is here to stop. The rule needs no upkeep:
# it cannot pass a wrong value, because the only pt value it accepts for
# a differing key is the correctly rebranded one, and a legitimately
# brand-specific value that is *not* a rename reddens and forces someone
# to say so out loud instead of diverging in silence.
BRAND_WORDS = (("Shitty", "Pretty"), ("shitty", "pretty"), ("SHITTY", "PRETTY"))


# Documentation divergences excused for now. Empty, and meant to stay
# that way: it exists so that a divergence too big to fix in the task
# that finds it can be named out loud instead of the check being
# switched off.
#
# The one entry it was born with is gone. `# CLI: -vulkanBlit ...`
# reached bin/st/shitty.toml in 92657e71 and never reached
# bin/pt/pretty.toml; F1 added the line and deleted the entry in the
# same edit, because either half alone reddens - the line without the
# deletion trips the staleness check below, and the deletion without the
# line trips the parity check itself.
#
# The shape, should a second one ever be needed: keyed by the file that
# is missing the name, and by the name, so a substitution is caught and
# not only growth - an entry excuses exactly one name missing from
# exactly one file. An entry naming a name that is documented again
# reddens as stale, so no allowance can outlive its reason.
DOCUMENTATION_ALLOWANCE = {}


def rebrand(value):
    """The st spelling of a value, rewritten for the pt brand."""
    for shitty_word, pretty_word in BRAND_WORDS:
        value = value.replace(shitty_word, pretty_word)
    return value


def read_example_config(path):
    """Assignments, documented option names and table headers of an
    example config.

    Assignments are returned as (key, raw value) pairs rather than a
    dict, so that a key assigned twice is visible to the caller instead
    of being swallowed by the second write; comparing them by name keeps
    the comparison blind to line order. Nothing here strips a trailing
    comment: no assignment in either file carries one, and every `#` in a
    value is inside a quoted color.

    Documented names come from the two shapes the files use to index an
    option: `# CLI: -name ...` for anything with a command-line form, and
    `# name — ...` for the config-only ones. Both are the documentation
    index rather than prose, and the parity check reads the names only,
    so rewording a description is not a divergence.
    """
    assignments = []
    documented = set()
    tables = []
    for line in path.read_text().splitlines():
        if line.startswith("# CLI: -"):
            documented.add(line.removeprefix("# CLI: -").split()[0])
            continue
        head, separator, _ = line.partition(" — ")
        if separator and head.startswith("# ") and head[2:].isidentifier():
            documented.add(head[2:])
            continue
        if line.startswith("["):
            tables.append(line)
            continue
        if line[:1].isalpha() or line[:1] == "_":
            key, separator, value = line.partition("=")
            if separator:
                assignments.append((key.strip(), value.strip()))
    return assignments, documented, tables


def config_home(directory, text):
    home = Path(directory) / "shitty"
    home.mkdir(parents=True)
    (home / "shitty.toml").write_text(text)
    return directory


def wait_for(expected, read):
    deadline = time.monotonic() + 2
    while True:
        value = read()
        if value == expected:
            return
        if time.monotonic() >= deadline:
            raise AssertionError(f"expected {expected!r}, got {value!r}")
        time.sleep(0.01)


class ExampleConfigParityTest(unittest.TestCase):
    """bin/st/shitty.toml and bin/pt/pretty.toml, kept in step.

    Two brands are built from one tree, and each ships its own example
    config. An option added to one file and forgotten in the other had no
    observer at all until this class: test_config.py opens only the st
    file, and production_surface.py only proves the pt binary *accepts*
    its file - a missing option leaves it perfectly valid. Measured by
    M8c on 2026-09-05: deleting an option from pretty.toml outright left
    every suite green.
    """

    def parsed(self):
        """Both files, with the premises that make comparing them mean
        something.

        Asserted before any comparison, because the failure this class is
        most exposed to is not a wrong answer but a vacuous one: two
        empty key sets compare equal, and a parser that matched nothing
        would report perfect parity on a tree that had lost half a file.
        """
        for path in (EXAMPLE_CONFIG, PRETTY_EXAMPLE_CONFIG):
            self.assertTrue(path.is_file(), f"{path} is missing")
            self.assertTrue(path.read_text().strip(), f"{path} is empty")

        result = {}
        for path in (EXAMPLE_CONFIG, PRETTY_EXAMPLE_CONFIG):
            pairs, documented, tables = read_example_config(path)
            # A key assigned twice would land in the dict once, and the
            # brand whose duplicate went missing would still compare
            # equal. Rare, but it is the shape that hides a divergence
            # from a set comparison, so it is refused outright.
            keys = [key for key, _ in pairs]
            duplicates = sorted({key for key in keys if keys.count(key) > 1})
            self.assertEqual(duplicates, [], f"{path} assigns these keys twice")
            assignments = dict(pairs)
            # A live table header scopes every key after it to that
            # table, and this parser is flat. Both files keep their only
            # tables commented out; the day one goes live, redden here
            # and teach the parser about scoping, rather than quietly
            # comparing keys from different tables as if they were one.
            self.assertEqual(tables, [], f"{path} grew a live TOML table")
            # Floors, not counts: the exact numbers are upstream's
            # business and change with every option. Anything this far
            # below today's 57 assignments and 79 documented names means
            # the parse failed, not that the file shrank.
            self.assertGreaterEqual(len(assignments), 20, f"{path}: parsed too few assignments")
            self.assertGreaterEqual(len(documented), 20, f"{path}: parsed too few documented options")
            # Anchors: three names every other test in this file leans
            # on. They prove the parser read assignments rather than
            # twenty lines of something else that happened to match.
            self.assertLessEqual({"fontsize", "title", "border"}, set(assignments), f"{path}")
            result[path] = (assignments, documented)
        return result

    def test_both_brands_assign_the_same_option_keys(self):
        parsed = self.parsed()
        shitty = set(parsed[EXAMPLE_CONFIG][0])
        pretty = set(parsed[PRETTY_EXAMPLE_CONFIG][0])
        self.assertSetEqual(
            shitty,
            pretty,
            "example configs disagree on which options they set; "
            f"only in bin/st/shitty.toml: {sorted(shitty - pretty)}; "
            f"only in bin/pt/pretty.toml: {sorted(pretty - shitty)}",
        )

    def test_both_brands_assign_the_same_values_up_to_the_brand_name(self):
        parsed = self.parsed()
        shitty = parsed[EXAMPLE_CONFIG][0]
        pretty = parsed[PRETTY_EXAMPLE_CONFIG][0]
        shared = sorted(set(shitty) & set(pretty))

        # The premise the brand rule needs, and the reason it is stated
        # rather than assumed: if no shared value differed literally, a
        # plain equality check would pass just as well, rebrand() would
        # never be exercised, and a rebrand() that had quietly become the
        # identity - an emptied BRAND_WORDS, a renamed brand - would go on
        # reporting parity forever. Naming the keys that carry the brand
        # makes the rule impossible to hollow out in silence.
        branded = [key for key in shared if shitty[key] != pretty[key]]
        self.assertTrue(
            branded,
            "no shared value differs between the brands, so the brand rule "
            "below is untested scaffolding; either a value was lost or "
            "BRAND_WORDS no longer describes how the brands differ",
        )
        self.assertTrue(
            [key for key in branded if rebrand(shitty[key]) != shitty[key]],
            "no differing value is touched by rebrand(), so BRAND_WORDS says "
            "nothing about how these two files differ and the check below "
            "would pass just as well with it emptied",
        )

        for key in shared:
            self.assertEqual(
                rebrand(shitty[key]),
                pretty[key],
                f"{key}: bin/pt/pretty.toml has {pretty[key]}, but the brand rule "
                f"says bin/st/shitty.toml's {shitty[key]} rebrands to "
                f"{rebrand(shitty[key])}",
            )

    def test_both_brands_document_the_same_options(self):
        parsed = self.parsed()
        documented = {
            "bin/st/shitty.toml": parsed[EXAMPLE_CONFIG][1],
            "bin/pt/pretty.toml": parsed[PRETTY_EXAMPLE_CONFIG][1],
        }
        everything = set().union(*documented.values())

        missing = {
            name: sorted(everything - names) for name, names in documented.items()
        }
        for name, allowed in DOCUMENTATION_ALLOWANCE.items():
            stale = sorted(allowed - set(missing[name]))
            self.assertEqual(
                stale,
                [],
                f"DOCUMENTATION_ALLOWANCE names {stale} as missing from {name}, "
                "but they are documented there now - delete the entry",
            )
        for name, names in missing.items():
            self.assertEqual(
                sorted(set(names) - DOCUMENTATION_ALLOWANCE.get(name, set())),
                [],
                f"{name} documents fewer options than its counterpart; "
                f"undocumented there: {sorted(set(names) - DOCUMENTATION_ALLOWANCE.get(name, set()))}",
            )


class ConfigFileTest(unittest.TestCase):
    def test_example_config_is_accepted_by_the_application(self):
        result = run_startup_failure(
            extra_arguments=("-config", EXAMPLE_CONFIG, "-version")
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, b"")

        with Shitty(pin_vga=False, extra_arguments=("-config", EXAMPLE_CONFIG)) as terminal:
            options = terminal.options()
            self.assertEqual(options["fontsize"], 15)
            # T8: the file stopped assigning fg, bg and cr and started
            # assigning colorScheme instead, so these three now read back
            # the named scheme's own colors. Asserted anyway, and against
            # literals: it is the one place the whole path - example
            # config, colorScheme lookup, palette, dump - is walked end
            # to end, and a file that had lost the key would answer with
            # the *other* scheme's colors rather than with nothing.
            self.assertEqual(options["fg"], 0xCDD6F4)
            self.assertEqual(options["bg"], 0x1E1E2E)
            self.assertEqual(options["cr"], 0xCDD6F4)

    def test_print_config_writes_the_brands_own_example_config(self):
        """-printConfig, both brands, against the file it is embedded from.

        Byte for byte, which is the whole design of the option: the
        printed file *is* bin/<brand>/<brand>.toml, embedded at build
        time, so a default edited in one place and forgotten in the other
        cannot exist. A generator reading optionsTable would need this
        check to hold it honest; embedding makes it a tautology worth
        asserting once, because the day someone replaces the embedding
        with a generator it stops being one.
        """
        for binary, example in ((SHITTY, EXAMPLE_CONFIG), (PRETTY, PRETTY_EXAMPLE_CONFIG)):
            with self.subTest(binary=binary.name):
                result = print_config(binary)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stderr, b"")
                # The premise: an option that printed nothing would pass
                # a comparison against an empty file, and Brand::generic()
                # really does return an empty config.
                self.assertGreater(len(result.stdout), 4000)
                self.assertEqual(result.stdout, example.read_bytes())

        # And the pt copy carries no trace of the other brand - the
        # binary now contains a whole text file that could.
        printed = print_config(PRETTY).stdout
        self.assertNotIn(b"shitty", printed.lower())

    def test_print_config_output_starts_the_terminal(self):
        # The claim the option makes in its own help text: redirect it
        # into the config path and the terminal comes up on it. Written
        # to a real file rather than piped, because that is what a user
        # does with it.
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "printed.toml"
            printed = print_config(SHITTY).stdout
            path.write_bytes(printed)

            result = run_startup_failure(
                extra_arguments=("-config", path, "-version")
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stderr, b"")

            # Started, and started on the same values the defaults give:
            # a config the parser accepted while ignoring every key would
            # pass the check above.
            with Shitty(pin_vga=False, extra_arguments=("-config", path)) as terminal:
                options = terminal.options()
                self.assertEqual(options["fontsize"], 15)
                self.assertEqual(options["background_opacity"], 60)
                self.assertEqual(options["background_blur"], "glass")
                # The example leaves tabs and panes to the binary's own
                # default, which for st on Linux is off (bin/st/main.cpp).
                bare = sys.platform.startswith("linux")
                self.assertEqual(options["sidebar_tabs"], 0 if bare else 1)
                self.assertEqual(options["panes"], 0 if bare else 1)
                self.assertEqual(options["pane_divider_color"], 0x00CD00)

    def test_example_config_documents_every_public_cli_option(self):
        listed = set()
        for argument in ("-help", "-listres"):
            result = run_startup_failure(extra_arguments=(argument,))
            self.assertEqual(result.returncode, 0)
            listed.update(
                match.decode()
                for match in re.findall(rb"^  -([^ ]+)", result.stdout, re.MULTILINE)
            )

        documented = {}
        for line in EXAMPLE_CONFIG.read_text().splitlines():
            if not line.startswith("# CLI: -"):
                continue
            syntax, separator, description = line.partition(" — ")
            self.assertEqual(separator, " — ", line)
            self.assertTrue(description, line)
            name = syntax.removeprefix("# CLI: -").split()[0]
            self.assertNotIn(name, documented, f"duplicate documentation for -{name}")
            documented[name] = description

        self.assertSetEqual(set(documented), listed)

    def test_import_loads_other_files_with_the_importer_on_top(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "themes").mkdir()
            (root / "themes" / "base.toml").write_text(
                'border = "7"\n'
                'fontsize = "18"\n'
            )
            (root / "themes" / "accent.toml").write_text(
                'border = "8"\n'
                'fontsize = "19"\n'
            )
            (root / "main.toml").write_text(
                'import = ["themes/base.toml", "themes/accent.toml"]\n'
                'fontsize = "23"\n'
            )
            with Shitty(
                pin_border=False,
                extra_arguments=("-config", root / "main.toml")
            ) as terminal:
                options = terminal.options()
                # A later import overrides an earlier one, and the
                # importing file overrides them all.
                self.assertEqual(options["border"], 8)
                self.assertEqual(options["fontsize"], 23)

    def test_import_nests_and_expands_home(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "inner.toml").write_text('border = "5"\n')
            (root / "middle.toml").write_text(
                'import = "~/inner.toml"\n'
                'fontsize = "21"\n'
            )
            (root / "main.toml").write_text('import = ["middle.toml"]\n')
            with Shitty(
                pin_border=False,
                extra_arguments=("-config", root / "main.toml"),
                extra_environment={"HOME": str(root)},
            ) as terminal:
                options = terminal.options()
                self.assertEqual(options["border"], 5)
                self.assertEqual(options["fontsize"], 21)

    def test_import_of_a_missing_file_fails_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "main.toml"
            config.write_text('import = ["nowhere.toml"]\n')
            result = run_startup_failure(
                extra_arguments=("-config", config)
            )
            self.assertEqual(result.returncode, 255)
            self.assertIn(b"config import: cannot open", result.stdout)

    def test_import_loop_hits_the_depth_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "main.toml"
            config.write_text('import = ["main.toml"]\n')
            result = run_startup_failure(
                extra_arguments=("-config", config)
            )
            self.assertEqual(result.returncode, 255)
            self.assertIn(b"nest deeper", result.stdout)

    def test_option_comes_from_the_default_config_path(self):
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, "fontsize = 33\n")
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(extra_environment=environment) as terminal:
                self.assertEqual(terminal.font_state()[0], 33)

    def test_command_line_beats_the_config(self):
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, "fontsize = 33\n")
            environment = {"XDG_CONFIG_HOME": directory}
            arguments = ("-fontsize", "22")
            with Shitty(extra_environment=environment, extra_arguments=arguments) as terminal:
                self.assertEqual(terminal.font_state()[0], 22)

    def test_quick_and_transparent_titlebar_take_their_defaults(self):
        # T8 split the pair: quick is still off, the titlebar is not.
        # Both are asserted, which is what keeps this from passing on a
        # dump that answered a constant to every key.
        with Shitty() as terminal:
            options = terminal.options()
            self.assertEqual(options["quick"], 0)
            self.assertEqual(options["transparent_titlebar"], 1)

    def test_quick_and_transparent_titlebar_come_from_the_config(self):
        # Exercises the wiring options_ut.cpp cannot: config -> Options ->
        # application.cpp's WindowOptions -> composer.opts -> the OPTIONS
        # protocol in test_mode.cpp.
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, "quick = true\ntransparentTitlebar = true\n")
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(extra_environment=environment) as terminal:
                options = terminal.options()
                self.assertEqual(options["quick"], 1)
                self.assertEqual(options["transparent_titlebar"], 1)

    def test_translucency_defaults_to_glass_over_a_translucent_window(self):
        # T8 turned this pair around: it used to promise the solid
        # window upstream draws, and now promises the translucent one
        # this fork ships. Both halves, for the reason the options_ut
        # twin gives - an opacity below 100 with no backdrop shows the
        # desktop raw, and a backdrop at 100 is what the startup warning
        # exists for, so either half alone is satisfiable by an accident.
        with Shitty() as terminal:
            options = terminal.options()
            self.assertEqual(options["background_opacity"], 60)
            self.assertEqual(options["background_blur"], "glass")

    def test_translucency_comes_from_the_config(self):
        # The wiring options_ut.cpp cannot reach: config -> Options ->
        # composer.opts -> the OPTIONS protocol. Until F10 the protocol
        # did not carry these two at all, so nothing in the black box
        # could tell a value that arrived from one that was dropped on
        # the way - in a wave whose result is otherwise judged by eye,
        # which is the worst place to have no machine-readable check.
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, "backgroundOpacity = 40\nbackgroundBlur = true\n")
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(extra_environment=environment) as terminal:
                options = terminal.options()
                self.assertEqual(options["background_opacity"], 40)
                self.assertEqual(options["background_blur"], "blur")

    def test_the_command_line_beats_a_configured_opacity(self):
        # The other half of the same wiring: a value that arrives late.
        # Without this, a protocol key wired to a constant would pass the
        # test above.
        #
        # `-backgroundBlur off` and not `+backgroundBlur`: T1 turned the
        # option into one that takes a value, and the '+' form it used to
        # accept is now refused outright. The config below spells the key
        # the old way on purpose - `true` survives as an alias, so this
        # pair also pins that a config written while it was a flag still
        # starts, and that the command line still wins over it.
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, "backgroundOpacity = 40\nbackgroundBlur = true\n")
            environment = {"XDG_CONFIG_HOME": directory}
            arguments = ("-backgroundOpacity", "70", "-backgroundBlur", "off")
            with Shitty(extra_environment=environment, extra_arguments=arguments) as terminal:
                options = terminal.options()
                self.assertEqual(options["background_opacity"], 70)
                self.assertEqual(options["background_blur"], "off")

    def test_blur_without_translucency_warns_and_still_starts(self):
        # F10. Blur over an opaque background draws nothing, on purpose -
        # but silence left the user with an option that appeared broken.
        # The line has to name the way out, not merely report the fact.
        with tempfile.TemporaryDirectory() as directory:
            # T8: opacity has to be asked for now. The default is 60,
            # and the warning this test is about is precisely the one
            # that is *not* due at 60 - so the config states the 100 it
            # used to inherit.
            config_home(directory, "backgroundBlur = true\nbackgroundOpacity = 100\n")
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(extra_environment=environment, capture_stderr=True) as terminal:
                # A warning and not a refusal: the terminal is up, and it
                # kept the option it warned about.
                self.assertEqual(terminal.options()["background_blur"], "blur")
            stderr = terminal.stderr_text()
            self.assertIn(b"-backgroundBlur", stderr)
            # The actionable half: which other option to reach for.
            self.assertIn(b"-backgroundOpacity", stderr)

    def test_the_backdrop_warning_speaks_of_showing_and_covers_both_modes(self):
        # R1-test. Two unobserved halves of one line. The wording moved
        # from "nothing to blur" to "nothing to show" when the option
        # grew a glass mode - blurring is the wrong verb for glass - and
        # the only assertions on this message were the two option names,
        # which survive any rewording, the old one included. And the
        # warning was proved for one mode only: a gate written
        # `== BackdropMode::Blur` would leave glass silent and redden
        # nothing.
        #
        # The dump assertion carries a third unobserved place with it.
        # "glass" is a name backdropModeName() alone decides, and until
        # now no test asked for it: the mode could have printed back as
        # "blur" all the way to the eye.
        for mode in ("blur", "glass"):
            with self.subTest(mode=mode):
                with tempfile.TemporaryDirectory() as directory:
                    # T8: 100 stated rather than inherited, the same as
                    # the test above. The premise below - that the
                    # warning was said at all - is what this line makes
                    # true again.
                    config_home(directory, f'backgroundBlur = "{mode}"\nbackgroundOpacity = 100\n')
                    environment = {"XDG_CONFIG_HOME": directory}
                    with Shitty(extra_environment=environment, capture_stderr=True) as terminal:
                        self.assertEqual(terminal.options()["background_blur"], mode)
                    stderr = terminal.stderr_text()
                    # The premise. Every assertion below is about the
                    # shape of a warning, and all of them pass on an
                    # empty stderr - which is the failure they exist to
                    # catch. The config above states an opacity of 100,
                    # so the warning is due; that it was said at all is asserted
                    # before anything about how it was said.
                    self.assertIn(b"-backgroundBlur", stderr)
                    self.assertIn(b"has nothing to show", stderr)
                    self.assertNotIn(b"to blur", stderr)

    def test_a_translucent_background_leaves_the_blur_warning_unsaid(self):
        # The control. Without it the assertions above would pass just as
        # well against a build that warns on every start.
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, "backgroundBlur = true\nbackgroundOpacity = 55\n")
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(extra_environment=environment, capture_stderr=True) as terminal:
                self.assertEqual(terminal.options()["background_opacity"], 55)
            self.assertNotIn(b"-backgroundBlur", terminal.stderr_text())

    def test_explicit_config_path_is_honored(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "own.toml"
            path.write_text("fontsize = 44\n")
            with Shitty(extra_arguments=("-config", path)) as terminal:
                self.assertEqual(terminal.font_state()[0], 44)

    def test_missing_explicit_config_fails_startup(self):
        result = run_startup_failure(extra_arguments=("-config", "/nonexistent/st.toml"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"-config", result.stdout + result.stderr)

    def test_broken_config_warns_but_the_terminal_starts(self):
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, "fontsize = 33\nboldColors = ???\n")
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(extra_environment=environment) as terminal:
                self.assertEqual(terminal.font_state()[0], 33)

    def test_hostile_config_shapes_warn_but_the_terminal_starts(self):
        # Every rejected TOML shape in one file: dotted keys, a list for
        # a scalar option, a scalar for a list option, non-string list
        # entries, inline tables, and an unterminated dollar expansion.
        text = (
            "a.b = 1\n"
            "fontsize = [1, 2]\n"
            "font = 'One Lone Face'\n"
            "font = [1, 2]\n"
            "border = {x = 1}\n"
            "title = 'tail dollar $'\n"
            "fontsize = 34\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, text)
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(extra_environment=environment) as terminal:
                self.assertEqual(terminal.font_state()[0], 34)

    def test_unknown_keys_and_tables_are_ignored(self):
        text = "nonsense = 1\n[section]\nx = 2\n"
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, "fontsize = 33\n" + text)
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(extra_environment=environment) as terminal:
                self.assertEqual(terminal.font_state()[0], 33)

    def test_typed_values_parse_alongside_the_applied_one(self):
        text = "boldColors = false\nborder = 7\ntitle = 'from config'\nfontsize = 27\n"
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, text)
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(pin_border=False, extra_environment=environment) as terminal:
                self.assertEqual(terminal.font_state()[0], 27)

    def test_environment_expands_in_the_config_body(self):
        text = "fontsize = ${SHITTY_TEST_WANTED_SIZE}\ntitle = '${HOME} of ${NO_SUCH_VARIABLE}'\n"
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, text)
            environment = {
                "XDG_CONFIG_HOME": directory,
                "SHITTY_TEST_WANTED_SIZE": "31",
            }
            with Shitty(extra_environment=environment) as terminal:
                self.assertEqual(terminal.font_state()[0], 31)

    def test_font_list_is_accepted(self):
        text = 'font = ["Test Font", "Fallback Font"]\nfontsize = 21\n'
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, text)
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(extra_environment=environment) as terminal:
                self.assertEqual(terminal.font_state()[0], 21)

    def test_uri_scheme_list_comes_from_the_config(self):
        text = 'uriScheme = ["gemini"]\n'
        control = 2
        with tempfile.TemporaryDirectory() as directory:
            config_home(directory, text)
            environment = {"XDG_CONFIG_HOME": directory}
            with Shitty(columns=64, rows=1, extra_environment=environment) as terminal:
                uri = b"gemini://example.test"
                terminal.write(uri + b" https://example.test")
                terminal.pointer(2 + 4, 2, modifiers=control)
                self.assertEqual(terminal.desktop_state()["icon"], 1)
                terminal.pointer(2 + len(uri) + 5, 2, modifiers=control)
                self.assertEqual(terminal.desktop_state()["icon"], 0)

    def test_sigusr1_reloads_the_snapshot_and_reapplies_vterm_state(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "live.toml"
            path.write_text("fontsize = 17\nborder = 3\ntitle = 'first'\n")
            with Shitty(pin_border=False, extra_arguments=("-config", path)) as terminal:
                self.assertEqual(terminal.font_state()[0], 17)
                self.assertEqual(terminal.window_title(), "first")

                path.write_text(
                    "fontsize = 23\n"
                    "border = 9\n"
                    "title = 'second'\n"
                    "remap = ['ctrl+b=ctrl+d']\n"
                )
                os.kill(terminal.process.pid, signal.SIGUSR1)

                wait_for(23, lambda: terminal.font_state()[0])
                self.assertEqual(terminal.font_state()[-1], 9)
                wait_for("second", terminal.window_title)
                terminal.layout_key("B", "b", "b", modifiers=2)
                self.assertEqual(terminal.read_input(), b"\x04")

                terminal.write(b"\x1b]2;runtime\x07")
                self.assertEqual(terminal.window_title(), "runtime")

                # A reload is an explicit reapplication, not an old/new
                # diff. The unchanged file therefore resets runtime Vterm
                # overrides to the configured snapshot too.
                os.kill(terminal.process.pid, signal.SIGUSR1)
                wait_for("second", terminal.window_title)


    def test_import_accepts_an_absolute_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "inner.toml").write_text('border = "5"\n')
            (root / "main.toml").write_text(
                f'import = ["{root / "inner.toml"}"]\nfontsize = "21"\n'
            )
            with Shitty(
                pin_border=False,
                extra_arguments=("-config", root / "main.toml")
            ) as terminal:
                options = terminal.options()
                self.assertEqual(options["border"], 5)
                self.assertEqual(options["fontsize"], 21)


    def test_a_reload_that_cannot_reopen_the_font_still_applies_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "live.toml"
            path.write_text("border = 3\n")
            with Shitty(pin_border=False, extra_arguments=("-config", path)) as terminal:
                path.write_text("border = 9\n")
                terminal.fail_next_font_change()
                os.kill(terminal.process.pid, signal.SIGUSR1)
                wait_for(9, lambda: terminal.options()["border"])
                terminal.write(b"still alive")
                self.assertTrue(
                    terminal.snapshot().lines[0].startswith("still alive")
                )


    def test_a_reload_that_fails_to_load_keeps_the_running_config(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "live.toml"
            path.write_text("border = 3\n")
            with Shitty(pin_border=False, extra_arguments=("-config", path)) as terminal:
                path.write_text('import = ["nowhere.toml"]\nborder = 9\n')
                os.kill(terminal.process.pid, signal.SIGUSR1)
                time.sleep(0.3)
                terminal.pump()
                self.assertEqual(terminal.options()["border"], 3)
                path.write_text("border = 7\n")
                os.kill(terminal.process.pid, signal.SIGUSR1)
                wait_for(7, lambda: terminal.options()["border"])


if __name__ == "__main__":
    unittest.main()
