/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "fuzzy.h"
#include "palette.h"
#include "ssh_hosts.h"

#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/tst/ut.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace stl;

namespace {
    void makeTempDir(StringBuilder& dir) {
        const char* const directory = getenv("TMPDIR");
        dir << StringView(directory != nullptr ? directory : "/tmp") << StringView(u8"/palette_ut.XXXXXX");
        STD_INSIST(mkdtemp(dir.cStr()) != nullptr);
    }

    void writeFile(StringView path, StringView text) {
        StringBuilder name;
        name << path;
        FILE* const file = fopen(name.cStr(), "w");
        STD_INSIST(file != nullptr);
        STD_INSIST(fwrite(text.data(), 1, text.length(), file) == text.length());
        STD_INSIST(fclose(file) == 0);
    }

    void makeDir(StringView path) {
        StringBuilder name;
        name << path;
        STD_INSIST(mkdir(name.cStr(), 0700) == 0);
    }

    void removeTree(StringView path) {
        StringBuilder command;
        command << StringView(u8"rm -rf '") << path << StringView(u8"'");
        STD_INSIST(system(command.cStr()) == 0);
    }

    PaletteItem item(PaletteKind kind, const char* title, const char* key = "", bool star = false) {
        PaletteItem made;
        made.kind = kind;
        made.title = StringView(title);
        made.key = StringView(key);
        made.star = star;
        return made;
    }

    // The rows as one line: headings in [brackets], items by title.
    void render(const Vector<PaletteRow>& rows, StringBuilder& out) {
        out.reset();
        for (const PaletteRow& row : rows) {
            if (row.heading) {
                out << StringView(u8"[") << row.title << StringView(u8"]");
            } else {
                out << StringView(u8" ") << row.title;
            }
        }
    }
}

STD_TEST_SUITE(Fuzzy) {
    // A run at a word's start beats the same letters strewn about, and
    // the matched offsets are where the letters are.
    STD_TEST(ARunAtAWordStartBeatsStrewnLetters) {
        Vector<size_t> at;
        const int word = fuzzyScore(StringView(u8"pro"), StringView(u8"api-proxy"), &at);
        STD_INSIST(at.length() == 3 && at[0] == 4 && at[1] == 5 && at[2] == 6);
        const int strewn = fuzzyScore(StringView(u8"pro"), StringView(u8"uplink-rollout"));
        // Premise: both match.
        STD_INSIST(word > 0 && strewn > 0);
        STD_INSIST(word > strewn);
        STD_INSIST(fuzzyScore(StringView(u8"PRO"), StringView(u8"prod")) == fuzzyScore(StringView(u8"pro"), StringView(u8"prod")));
        STD_INSIST(fuzzyScore(StringView(u8"xyz"), StringView(u8"prod")) < 0);
        STD_INSIST(fuzzyScore(StringView(), StringView(u8"prod")) == 0);
        // The same run of letters, once after a boundary and once inside a
        // word: only the word start tells them apart.
        STD_INSIST(fuzzyScore(StringView(u8"db"), StringView(u8"prod-db")) > fuzzyScore(StringView(u8"db"), StringView(u8"prodxdb")));
        // The start of the name is worth more than a later word start.
        STD_INSIST(fuzzyScore(StringView(u8"db"), StringView(u8"db-prod")) > fuzzyScore(StringView(u8"db"), StringView(u8"prod-db")));
    }
}

STD_TEST_SUITE(SshHosts) {
    // Concrete names once each in order, patterns left out, the first
    // value of a key kept, Match ending a block, key=value accepted.
    STD_TEST(NamesAreReadOnceWithTheirFirstValues) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Vector<SshHost> hosts;
        sshConfigHosts(StringView(u8"# mine\n"
                                  "Host prod prod-alt\n"
                                  "  HostName 10.0.0.1\n"
                                  "  User=root\n"
                                  "Host *.internal !bad\n"
                                  "  User nobody\n"
                                  "Host prod\n"
                                  "  HostName 10.9.9.9\n"
                                  "  Port 2222\n"
                                  "Match host x\n"
                                  "  User ignored\n"
                                  "host \"lab box\"\n"),
                       *pool, hosts);
        STD_INSIST(hosts.length() == 3);
        STD_INSIST(hosts[0].alias == StringView(u8"prod") && hosts[0].hostName == StringView(u8"10.0.0.1") && hosts[0].user == StringView(u8"root") && hosts[0].port == StringView(u8"2222"));
        STD_INSIST(hosts[1].alias == StringView(u8"prod-alt") && hosts[1].user == StringView(u8"root") && hosts[1].port.empty());
        STD_INSIST(hosts[2].alias == StringView(u8"lab box") && hosts[2].user.empty());
    }

    // Include, relative to ~/.ssh and globbed, brings its hosts in.
    STD_TEST(IncludedFilesAddTheirHosts) {
        StringBuilder home;
        makeTempDir(home);
        StringBuilder ssh;
        ssh << StringView(home) << StringView(u8"/.ssh");
        makeDir(StringView(ssh));
        StringBuilder sub;
        sub << StringView(ssh) << StringView(u8"/conf.d");
        makeDir(StringView(sub));
        StringBuilder main;
        main << StringView(ssh) << StringView(u8"/config");
        writeFile(StringView(main), StringView(u8"Host a\nInclude conf.d/*.conf\nHost b\n"));
        StringBuilder one;
        one << StringView(sub) << StringView(u8"/one.conf");
        writeFile(StringView(one), StringView(u8"Host inc1\n  User u\n"));
        StringBuilder two;
        two << StringView(sub) << StringView(u8"/two.conf");
        writeFile(StringView(two), StringView(u8"Host inc2\n"));
        ObjPool::Ref pool = ObjPool::fromMemory();
        Vector<SshHost> hosts;
        sshConfigHostsFromHome(StringView(home), *pool, hosts);
        StringBuilder names;
        for (const SshHost& host : hosts) {
            names << host.alias << StringView(u8" ");
        }
        STD_INSIST(StringView(names) == StringView(u8"a inc1 inc2 b "));
        STD_INSIST(hosts[1].user == StringView(u8"u"));
        removeTree(StringView(home));
    }

    // tsh's JSON: each node's hostname, address and labels; anything
    // broken gives nothing rather than half a list.
    STD_TEST(TeleportNodesAreReadFromItsJson) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Vector<TeleportHost> hosts;
        teleportHosts(StringView(u8"[{\"kind\":\"node\",\"version\":\"v2\",\"metadata\":{\"name\":\"9f1\",\"labels\":{\"env\":\"prod\",\"role\":\"db\"}},"
                                 "\"spec\":{\"addr\":\"10.0.0.5:3022\",\"hostname\":\"prod-db-1\",\"rotation\":{\"current_id\":\"\"},\"public_addrs\":[]}},"
                                 "{\"kind\":\"node\",\"metadata\":{\"name\":\"9f2\"},\"spec\":{\"hostname\":\"web\\\"2\",\"addr\":\"\"}}]"),
                      *pool, hosts);
        STD_INSIST(hosts.length() == 2);
        STD_INSIST(hosts[0].hostname == StringView(u8"prod-db-1") && hosts[0].address == StringView(u8"10.0.0.5:3022"));
        STD_INSIST(hosts[0].labels == StringView(u8"env=prod · role=db"));
        STD_INSIST(hosts[1].hostname == StringView(u8"web\"2") && hosts[1].labels.empty());
        Vector<TeleportHost> none;
        teleportHosts(StringView(u8"ERROR: not logged in"), *pool, none);
        teleportHosts(StringView(u8"[{\"spec\":{\"hostname\":\"x\"}"), *pool, none);
        STD_INSIST(none.empty());
    }
}

STD_TEST_SUITE(Palette) {
    // Each prefix picks its mode and is dropped; ~ stays, being a path.
    STD_TEST(PrefixesPickTheMode) {
        StringView rest;
        STD_INSIST(paletteMode(StringView(u8"@ kub"), rest) == PaletteMode::Hosts && rest == StringView(u8"kub"));
        STD_INSIST(paletteMode(StringView(u8">clone x"), rest) == PaletteMode::Actions && rest == StringView(u8"clone x"));
        STD_INSIST(paletteMode(StringView(u8"/ ~/Pro"), rest) == PaletteMode::Folders && rest == StringView(u8"~/Pro"));
        STD_INSIST(paletteMode(StringView(u8"~/Pro"), rest) == PaletteMode::Folders && rest == StringView(u8"~/Pro"));
        STD_INSIST(paletteMode(StringView(u8"!lazy"), rest) == PaletteMode::Apps && rest == StringView(u8"lazy"));
        STD_INSIST(paletteMode(StringView(u8"$prod"), rest) == PaletteMode::Env && rest == StringView(u8"prod"));
        STD_INSIST(paletteMode(StringView(u8"prod"), rest) == PaletteMode::All && rest == StringView(u8"prod"));
    }

    // A typed query: sections in their order, best first in each, a
    // bookmark ahead of an equal match; a prefix keeps only its kind.
    STD_TEST(MatchesAreSectionedAndRanked) {
        Vector<PaletteItem> items;
        items.pushBack(item(PaletteKind::Action, "Clone Repository…", "action:clone"));
        items.pushBack(item(PaletteKind::SshHost, "prometheus", "ssh:prometheus"));
        items.pushBack(item(PaletteKind::SshHost, "prod", "ssh:prod"));
        items.pushBack(item(PaletteKind::Folder, "~/Projects/RWB", "dir:/h/Projects/RWB"));
        items.pushBack(item(PaletteKind::Bookmark, "prod box", "bm:prod box", true));
        items.pushBack(item(PaletteKind::Env, "prod-eu", "env:prod-eu"));
        items.pushBack(item(PaletteKind::App, "htop", "app:htop"));
        const Vector<StringView> recents;
        Vector<PaletteRow> rows;
        StringBuilder text;
        paletteQuery(items, StringView(u8"pro"), recents, rows);
        render(rows, text);
        STD_INSIST(StringView(text) == StringView(u8"[Bookmarks] prod box[SSH Hosts] prod prometheus[Folders] ~/Projects/RWB[Environments] prod-eu"));
        // The matched letters of the first host.
        STD_INSIST(rows.mut(3).markCount == 3 && rows.mut(3).marks[0] == 0);
        paletteQuery(items, StringView(u8"@pro"), recents, rows);
        render(rows, text);
        STD_INSIST(StringView(text) == StringView(u8"[SSH Hosts] prod prometheus"));
        // A recent pick moves ahead of a better match.
        Vector<StringView> recent;
        recent.pushBack(StringView(u8"ssh:prometheus"));
        paletteQuery(items, StringView(u8"@pro"), recent, rows);
        render(rows, text);
        STD_INSIST(StringView(text) == StringView(u8"[SSH Hosts] prometheus prod"));
    }

    // Nothing typed: the recents in their order, then the actions; the
    // cursor steps over headings and round the ends.
    STD_TEST(NothingTypedShowsRecentsThenActions) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        Vector<PaletteItem> items;
        items.pushBack(item(PaletteKind::SshHost, "a", "ssh:a"));
        items.pushBack(item(PaletteKind::SshHost, "b", "ssh:b"));
        paletteActions(*pool, items);
        Vector<StringView> recents;
        recents.pushBack(StringView(u8"ssh:b"));
        recents.pushBack(StringView(u8"ssh:gone"));
        recents.pushBack(StringView(u8"ssh:a"));
        Vector<PaletteRow> rows;
        paletteQuery(items, StringView(), recents, rows);
        StringBuilder text;
        render(rows, text);
        STD_INSIST(StringView(text) == StringView(u8"[Recent] b a[Actions] New Tab Open Folder… Clone Repository…"));
        STD_INSIST(paletteFirstItem(rows) == 1);
        STD_INSIST(paletteStep(rows, 2, 1) == 4);
        STD_INSIST(paletteStep(rows, 1, -1) == rows.length() - 1);
        STD_INSIST(paletteStep(rows, rows.length() - 1, 1) == 1);
    }

    // A typed clone: the URL, the repository's folder, and the commands
    // a new tab and the current tab get, every word quoted.
    STD_TEST(ACloneIsTypedAndPlanned) {
        ObjPool::Ref pool = ObjPool::fromMemory();
        PaletteItem clone;
        STD_INSIST(!paletteCloneItem(StringView(u8"> clone "), StringView(u8"/h/Projects"), *pool, clone));
        STD_INSIST(!paletteCloneItem(StringView(u8"clone x"), StringView(u8"/h/Projects"), *pool, clone));
        STD_INSIST(paletteCloneItem(StringView(u8"> clone git@github.com:pg83/shitty.git"), StringView(u8"/h/Projects/"), *pool, clone));
        STD_INSIST(clone.directory == StringView(u8"/h/Projects/shitty") && clone.command == StringView(u8"git@github.com:pg83/shitty.git"));
        PalettePlan plan;
        palettePlan(clone, PaletteTarget::NewTab, StringView(), plan);
        STD_INSIST(StringView(plan.command) == StringView(u8"git clone 'git@github.com:pg83/shitty.git' '/h/Projects/shitty' && cd '/h/Projects/shitty' && exec \"${SHELL:-/bin/sh}\""));
        STD_INSIST(StringView(plan.title) == StringView(u8"shitty") && StringView(plan.directory).empty());
        palettePlan(clone, PaletteTarget::CurrentTab, StringView(), plan);
        STD_INSIST(StringView(plan.typed) == StringView(u8"git clone 'git@github.com:pg83/shitty.git' '/h/Projects/shitty' && cd '/h/Projects/shitty'"));
        STD_INSIST(StringView(plan.command).empty());
    }

    // What each kind runs, in a new tab and typed into the current one;
    // a quote in a value cannot end the quoting.
    STD_TEST(EachKindIsPlannedForEachTarget) {
        PalettePlan plan;
        PaletteItem host = item(PaletteKind::SshHost, "prod");
        palettePlan(host, PaletteTarget::NewTab, StringView(), plan);
        STD_INSIST(StringView(plan.command) == StringView(u8"ssh 'prod'"));
        PaletteItem node = item(PaletteKind::TeleportHost, "db-1");
        palettePlan(node, PaletteTarget::NewTab, StringView(u8"root"), plan);
        STD_INSIST(StringView(plan.command) == StringView(u8"tsh ssh 'root@db-1'"));
        PaletteItem folder = item(PaletteKind::Folder, "~/it's");
        folder.directory = StringView(u8"/h/it's");
        palettePlan(folder, PaletteTarget::NewTab, StringView(), plan);
        STD_INSIST(StringView(plan.command).empty() && StringView(plan.directory) == StringView(u8"/h/it's"));
        palettePlan(folder, PaletteTarget::CurrentTab, StringView(), plan);
        STD_INSIST(StringView(plan.typed) == StringView(u8"cd '/h/it'\\''s'"));
        PaletteItem env = item(PaletteKind::Env, "prod");
        env.variables = StringView(u8"AWS_PROFILE=prod\nKUBECONFIG=~/.kube/eu\nbroken\n");
        palettePlan(env, PaletteTarget::NewTab, StringView(), plan);
        STD_INSIST(StringView(plan.command) == StringView(u8"export AWS_PROFILE='prod' KUBECONFIG='~/.kube/eu'; exec \"${SHELL:-/bin/sh}\""));
        palettePlan(env, PaletteTarget::CurrentTab, StringView(), plan);
        STD_INSIST(StringView(plan.typed) == StringView(u8"export AWS_PROFILE='prod' KUBECONFIG='~/.kube/eu'"));
        PaletteItem app = item(PaletteKind::App, "lazygit");
        app.command = StringView(u8"lazygit");
        app.directory = StringView(u8"/h/p");
        palettePlan(app, PaletteTarget::CurrentTab, StringView(), plan);
        STD_INSIST(StringView(plan.typed) == StringView(u8"cd '/h/p' && lazygit"));
    }

    // In / mode a path lists the directories it starts, files and hidden
    // ones left out, and Tab completes to what they share.
    STD_TEST(APathListsAndCompletesDirectories) {
        StringBuilder home;
        makeTempDir(home);
        // Made in an order the listing does not keep: the sort is by name,
        // a longer name after its own start.
        const char* const names[] = {"/Projects", "/Pro", "/Programs", "/Pictures", "/.hidden"};
        for (const char* name : names) {
            StringBuilder path;
            path << StringView(home) << StringView(name);
            makeDir(StringView(path));
        }
        StringBuilder file;
        file << StringView(home) << StringView(u8"/Profile.txt");
        writeFile(StringView(file), StringView(u8"x"));
        ObjPool::Ref pool = ObjPool::fromMemory();
        Vector<PaletteItem> items;
        paletteDirectoryItems(StringView(u8"~/Pro"), StringView(home), *pool, items);
        STD_INSIST(items.length() == 3);
        STD_INSIST(items[0].title == StringView(u8"~/Pro") && items[1].title == StringView(u8"~/Programs") && items[2].title == StringView(u8"~/Projects"));
        StringBuilder expected;
        expected << StringView(home) << StringView(u8"/Projects");
        STD_INSIST(items[2].directory == StringView(expected));
        // Shown in / mode as the only folders.
        items.pushBack(item(PaletteKind::Folder, "~/Projects/RWB", "dir:/x"));
        Vector<PaletteRow> rows;
        const Vector<StringView> recents;
        paletteQuery(items, StringView(u8"/ ~/Pro"), recents, rows);
        StringBuilder text;
        render(rows, text);
        STD_INSIST(StringView(text) == StringView(u8"[Folders] ~/Pro ~/Programs ~/Projects"));
        StringBuilder completed;
        paletteComplete(StringView(u8"~/Pro"), StringView(home), completed);
        STD_INSIST(StringView(completed) == StringView(u8"~/Pro"));
        paletteComplete(StringView(u8"~/Proj"), StringView(home), completed);
        STD_INSIST(StringView(completed) == StringView(u8"~/Projects/"));
        paletteComplete(StringView(u8"~/Pi"), StringView(home), completed);
        STD_INSIST(StringView(completed) == StringView(u8"~/Pictures/"));
        removeTree(StringView(home));
    }

    // The recents file: newest first, once each, at most eight, actions
    // never; read back as written.
    STD_TEST(RecentsAreKeptNewestFirst) {
        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder path;
        path << StringView(dir) << StringView(u8"/palette_recent");
        ObjPool::Ref pool = ObjPool::fromMemory();
        Vector<StringView> recents;
        for (int n = 0; n < 10; ++n) {
            StringBuilder key;
            key << StringView(u8"ssh:h") << (i64)(n);
            paletteRemember(StringView(path), StringView(key), recents, *pool);
        }
        paletteRemember(StringView(path), StringView(u8"ssh:h5"), recents, *pool);
        paletteRemember(StringView(path), StringView(u8"action:new-tab"), recents, *pool);
        Vector<StringView> read;
        paletteLoadRecents(StringView(path), *pool, read);
        STD_INSIST(read.length() == 8);
        STD_INSIST(read[0] == StringView(u8"ssh:h5") && read[1] == StringView(u8"ssh:h9") && read[7] == StringView(u8"ssh:h2"));
        removeTree(StringView(dir));
    }
    // An empty list names where its rows come from: the table to write
    // and the file, home as ~; typed text that matched nothing says so.
    STD_TEST(AnEmptyListSaysWhereItsRowsComeFrom) {
        PaletteHint apps;
        PaletteHint envs;
        STD_INSIST(paletteEmptyHint(PaletteMode::Apps, StringView(), StringView(u8"/h/u/.config/t/t.toml"), StringView(u8"/h/u"), apps));
        STD_INSIST(paletteEmptyHint(PaletteMode::Env, StringView(), StringView(u8"/h/u/.config/t/t.toml"), StringView(u8"/h/u"), envs));
        // Premise: the two hints are told apart.
        STD_INSIST(StringView(apps.lines[0]) != StringView(envs.lines[0]));
        STD_INSIST(StringView(apps.lines[0]) == StringView(u8"Add [[app]] tables to"));
        STD_INSIST(apps.example[0] == StringView(u8"[[app]]"));
        STD_INSIST(StringView(envs.lines[0]) == StringView(u8"Add [[env]] tables to"));
        STD_INSIST(envs.example[0] == StringView(u8"[[env]]"));
        STD_INSIST(StringView(apps.lines[1]) == StringView(u8"~/.config/t/t.toml"));
        // A path outside home is shown whole, a home that only prefixes
        // a name is not home.
        paletteEmptyHint(PaletteMode::Apps, StringView(), StringView(u8"/h/user2/t.toml"), StringView(u8"/h/user"), apps);
        STD_INSIST(StringView(apps.lines[1]) == StringView(u8"/h/user2/t.toml"));
        PaletteHint none;
        STD_INSIST(paletteEmptyHint(PaletteMode::Apps, StringView(u8"lazy"), StringView(), StringView(), none));
        STD_INSIST(StringView(none.title) == StringView(u8"Nothing matches \u201clazy\u201d"));
        STD_INSIST(StringView(none.lines[0]).empty() && none.example[0].empty());
        STD_INSIST(!paletteEmptyHint(PaletteMode::All, StringView(), StringView(), StringView(), none));
    }
}
