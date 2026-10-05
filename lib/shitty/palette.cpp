/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "palette.h"

#include "fuzzy.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace stl;

namespace {
    constexpr size_t sectionCount = 6;
    constexpr size_t recentsKept = 8;

    const char* const sectionTitles[sectionCount] = {"Bookmarks", "SSH Hosts", "Folders", "Apps", "Environments", "Actions"};

    size_t sectionOf(const PaletteItem& item) {
        switch (item.kind) {
            case PaletteKind::Bookmark:
                return 0;
            case PaletteKind::SshHost:
            case PaletteKind::TeleportHost:
                return 1;
            case PaletteKind::Folder:
                return 2;
            case PaletteKind::App:
                return 3;
            case PaletteKind::Env:
                return 4;
            case PaletteKind::Action:
                return 5;
        }
        return 5;
    }

    bool inMode(const PaletteItem& item, PaletteMode mode) {
        switch (mode) {
            case PaletteMode::All:
                return true;
            case PaletteMode::Hosts:
                // A bookmark that runs something is most often a host.
                return item.kind == PaletteKind::SshHost || item.kind == PaletteKind::TeleportHost || (item.kind == PaletteKind::Bookmark && !item.command.empty());
            case PaletteMode::Folders:
                return item.kind == PaletteKind::Folder || (item.kind == PaletteKind::Bookmark && item.command.empty());
            case PaletteMode::Apps:
                return item.kind == PaletteKind::App;
            case PaletteMode::Env:
                return item.kind == PaletteKind::Env;
            case PaletteMode::Actions:
                return item.kind == PaletteKind::Action;
        }
        return false;
    }

    // How recent a key is: 0 for none, recentsKept for the latest.
    size_t recency(const Vector<StringView>& recents, StringView key) {
        if (key.empty()) {
            return 0;
        }
        for (size_t at = 0; at < recents.length(); ++at) {
            if (recents[at] == key) {
                return recentsKept - (at < recentsKept ? at : recentsKept - 1);
            }
        }
        return 0;
    }

    struct Scored {
        size_t item;
        int score;
    };

    void heading(Vector<PaletteRow>& out, const char* title) {
        PaletteRow row;
        row.heading = true;
        row.title = StringView(title);
        out.pushBack(row);
    }

    void itemRow(Vector<PaletteRow>& out, const PaletteItem& item, size_t index, StringView query) {
        PaletteRow row;
        row.item = index;
        row.title = item.title;
        if (!query.empty()) {
            Vector<size_t> positions;
            fuzzyScore(query, item.title, &positions);
            for (const size_t at : positions) {
                if (row.markCount < paletteMarksMax) {
                    row.marks[row.markCount++] = (u16)(at);
                }
            }
        }
        out.pushBack(row);
    }

    bool startsWith(StringView text, StringView start) {
        return text.length() >= start.length() && StringView(text.data(), start.length()) == start;
    }

    // ~/x → home/x, the rest as it is.
    void expand(StringView typed, StringView home, StringBuilder& out) {
        out.reset();
        if (startsWith(typed, StringView(u8"~")) && (typed.length() == 1 || typed.data()[1] == '/')) {
            out << home << StringView(typed.data() + 1, typed.length() - 1);
        } else {
            out << typed;
        }
    }

    // The directories in the directory part of `typed` whose names start
    // with its last part, sorted; each as the full path and as shown.
    void listMatches(StringView typed, StringView home, ObjPool& pool, Vector<StringView>& full, Vector<StringView>& shown) {
        full.clear();
        shown.clear();
        StringBuilder path;
        expand(typed, home, path);
        const StringView expanded(path);
        size_t slash = expanded.length();
        while (slash > 0 && expanded.data()[slash - 1] != '/') {
            --slash;
        }
        if (slash == 0) {
            return;
        }
        const StringView directory(expanded.data(), slash);
        const StringView base(expanded.data() + slash, expanded.length() - slash);
        const StringView typedDirectory(typed.data(), typed.length() - base.length());
        StringBuilder open;
        open << directory;
        DIR* const listing = opendir(open.cStr());
        if (listing == nullptr) {
            return;
        }
        Vector<StringView> names;
        while (const dirent* const entry = readdir(listing)) {
            const StringView name(entry->d_name);
            if (name == StringView(u8".") || name == StringView(u8"..")) {
                continue;
            }
            if (name.data()[0] == '.' && (base.empty() || base.data()[0] != '.')) {
                continue;
            }
            bool fits = name.length() >= base.length();
            for (size_t at = 0; fits && at < base.length(); ++at) {
                u8 a = (u8)(name.data()[at]);
                u8 b = (u8)(base.data()[at]);
                a = a >= 'A' && a <= 'Z' ? (u8)(a - 'A' + 'a') : a;
                b = b >= 'A' && b <= 'Z' ? (u8)(b - 'A' + 'a') : b;
                fits = a == b;
            }
            if (!fits) {
                continue;
            }
            StringBuilder candidate;
            candidate << directory << name;
            struct stat info;
            if (stat(candidate.cStr(), &info) != 0 || !S_ISDIR(info.st_mode)) {
                continue;
            }
            names.pushBack(pool.intern(name));
        }
        closedir(listing);
        // Insertion sort by bytes: a directory listing is short.
        const auto before = [](StringView a, StringView b) {
            const size_t n = a.length() < b.length() ? a.length() : b.length();
            for (size_t at = 0; at < n; ++at) {
                if (a.data()[at] != b.data()[at]) {
                    return (u8)(a.data()[at]) < (u8)(b.data()[at]);
                }
            }
            return a.length() < b.length();
        };
        for (size_t i = 1; i < names.length(); ++i) {
            for (size_t j = i; j > 0 && before(names[j], names[j - 1]); --j) {
                const StringView swap = names[j];
                names.mut(j) = names[j - 1];
                names.mut(j - 1) = swap;
            }
        }
        for (const StringView name : names) {
            StringBuilder f;
            f << directory << name;
            full.pushBack(pool.intern(StringView(f)));
            StringBuilder s;
            s << typedDirectory << name;
            shown.pushBack(pool.intern(StringView(s)));
        }
    }
}

PaletteMode paletteMode(StringView text, StringView& rest) {
    PaletteMode mode = PaletteMode::All;
    size_t skip = 0;
    if (!text.empty()) {
        switch (text.data()[0]) {
            case '>':
                mode = PaletteMode::Actions;
                break;
            case '@':
                mode = PaletteMode::Hosts;
                break;
            case '!':
                mode = PaletteMode::Apps;
                break;
            case '$':
                mode = PaletteMode::Env;
                break;
            case '/':
            case '~':
                mode = PaletteMode::Folders;
                break;
            default:
                break;
        }
        // Every prefix but ~ is dropped: "/ ~/Pro" and "/pro" search
        // folders, "~/Pro" alone is already a path.
        if (mode != PaletteMode::All && text.data()[0] != '~') {
            skip = 1;
        }
    }
    while (skip < text.length() && text.data()[skip] == ' ') {
        ++skip;
    }
    rest = StringView(text.data() + skip, text.length() - skip);
    return mode;
}

StringView palettePrefix(PaletteMode mode) {
    switch (mode) {
        case PaletteMode::Actions:
            return StringView(u8">");
        case PaletteMode::Hosts:
            return StringView(u8"@");
        case PaletteMode::Folders:
            return StringView(u8"/");
        case PaletteMode::Apps:
            return StringView(u8"!");
        case PaletteMode::Env:
            return StringView(u8"$");
        case PaletteMode::All:
            break;
    }
    return StringView();
}

StringView paletteModeName(PaletteMode mode) {
    switch (mode) {
        case PaletteMode::All:
            return StringView(u8"All");
        case PaletteMode::Actions:
            return StringView(u8"Actions");
        case PaletteMode::Hosts:
            return StringView(u8"Hosts");
        case PaletteMode::Folders:
            return StringView(u8"Folders");
        case PaletteMode::Apps:
            return StringView(u8"Apps");
        case PaletteMode::Env:
            return StringView(u8"Env");
    }
    return StringView();
}

void paletteQuery(const Vector<PaletteItem>& items, StringView text, const Vector<StringView>& recents, Vector<PaletteRow>& out, size_t perSection) {
    out.clear();
    StringView query;
    const PaletteMode mode = paletteMode(text, query);
    // A path typed in / mode is matched by the directory items the caller
    // listed for it (paletteDirectoryItems), not as a fuzzy query.
    const bool path = mode == PaletteMode::Folders && !query.empty() && (query.data()[0] == '/' || query.data()[0] == '~');
    if (mode == PaletteMode::All && query.empty()) {
        // Nothing typed: what was picked lately, then what can be done.
        bool any = false;
        for (const StringView key : recents) {
            for (size_t at = 0; at < items.length(); ++at) {
                if (items[at].key == key) {
                    if (!any) {
                        heading(out, "Recent");
                        any = true;
                    }
                    itemRow(out, items[at], at, StringView());
                    break;
                }
            }
        }
        bool actions = false;
        for (size_t at = 0; at < items.length(); ++at) {
            if (items[at].kind == PaletteKind::Action && items[at].action != PaletteAction::CloneUrl) {
                if (!actions) {
                    heading(out, sectionTitles[5]);
                    actions = true;
                }
                itemRow(out, items[at], at, StringView());
            }
        }
        return;
    }
    Vector<Scored> sections[sectionCount];
    for (size_t at = 0; at < items.length(); ++at) {
        const PaletteItem& item = items[at];
        if (!inMode(item, mode)) {
            continue;
        }
        int score = 0;
        if (path) {
            if (item.kind != PaletteKind::Folder || !startsWith(item.key, StringView(u8"path:"))) {
                continue;
            }
        } else if (item.action == PaletteAction::CloneUrl) {
            // Shown above everything while a clone is being typed.
            score = 1 << 20;
        } else if (!query.empty()) {
            score = fuzzyScore(query, item.title);
            if (score < 0) {
                score = fuzzyScore(query, item.subtitle);
                if (score < 0) {
                    continue;
                }
                score -= 40;
            }
        }
        if (item.star) {
            score += 40;
        }
        score += (int)(recency(recents, item.key)) * 4;
        sections[sectionOf(item)].pushBack(Scored{at, score});
    }
    for (size_t section = 0; section < sectionCount; ++section) {
        Vector<Scored>& list = sections[section];
        if (list.empty()) {
            continue;
        }
        // Best first; of equals the shorter name, being nearer what was
        // typed; then the items' own order.
        const auto ahead = [&](const Scored& a, const Scored& b) {
            return a.score > b.score || (a.score == b.score && items[a.item].title.length() < items[b.item].title.length());
        };
        for (size_t i = 1; i < list.length(); ++i) {
            for (size_t j = i; j > 0 && ahead(list[j], list[j - 1]); --j) {
                const Scored swap = list[j];
                list.mut(j) = list[j - 1];
                list.mut(j - 1) = swap;
            }
        }
        heading(out, sectionTitles[section]);
        const size_t shown = mode == PaletteMode::All && list.length() > perSection ? perSection : list.length();
        for (size_t at = 0; at < shown; ++at) {
            const PaletteItem& shown = items[list[at].item];
            itemRow(out, shown, list[at].item, path || shown.action == PaletteAction::CloneUrl ? StringView() : query);
        }
    }
}

float paletteRowTop(const Vector<PaletteRow>& rows, size_t index) {
    float top = PaletteMetrics::pad;
    for (size_t at = 0; at < index && at < rows.length(); ++at) {
        top += rows[at].heading ? PaletteMetrics::heading : PaletteMetrics::row;
    }
    return top;
}

long long paletteRowAt(const Vector<PaletteRow>& rows, float y, float scroll) {
    float top = PaletteMetrics::pad - scroll;
    for (size_t at = 0; at < rows.length(); ++at) {
        const float height = rows[at].heading ? PaletteMetrics::heading : PaletteMetrics::row;
        if (y >= top && y < top + height) {
            return rows[at].heading ? -1 : (long long)(at);
        }
        top += height;
    }
    return -1;
}

float paletteScrollTo(const Vector<PaletteRow>& rows, size_t index, float scroll, float height) {
    if (index >= rows.length()) {
        return 0;
    }
    float top = paletteRowTop(rows, index) - PaletteMetrics::pad;
    // The heading over the first row of a section comes into view with it.
    if (index > 0 && rows[index - 1].heading) {
        top -= PaletteMetrics::heading;
    }
    const float bottom = paletteRowTop(rows, index) + PaletteMetrics::row + PaletteMetrics::pad;
    if (top < scroll) {
        return top < 0 ? 0 : top;
    }
    if (bottom > scroll + height) {
        return bottom - height;
    }
    return scroll;
}

size_t paletteFirstItem(const Vector<PaletteRow>& rows) {
    for (size_t at = 0; at < rows.length(); ++at) {
        if (!rows[at].heading) {
            return at;
        }
    }
    return rows.length();
}

size_t paletteStep(const Vector<PaletteRow>& rows, size_t row, int step) {
    const size_t count = rows.length();
    if (count == 0) {
        return 0;
    }
    size_t at = row < count ? row : 0;
    for (size_t tried = 0; tried < count; ++tried) {
        at = step > 0 ? (at + 1) % count : (at + count - 1) % count;
        if (!rows[at].heading) {
            return at;
        }
    }
    return row;
}

void paletteActions(ObjPool& pool, Vector<PaletteItem>& out) {
    (void)(pool);
    const auto add = [&](PaletteAction action, const char* title, const char* subtitle, const char* key) {
        PaletteItem item;
        item.kind = PaletteKind::Action;
        item.action = action;
        item.title = StringView(title);
        item.subtitle = StringView(subtitle);
        item.key = StringView(key);
        out.pushBack(item);
    };
    add(PaletteAction::NewTab, "New Tab", "a shell where this one is", "action:new-tab");
    add(PaletteAction::OpenFolder, "Open Folder…", "in a new tab", "action:open-folder");
    add(PaletteAction::Clone, "Clone Repository…", "git clone, then open it", "action:clone");
}

bool paletteCloneItem(StringView text, StringView cloneDirectory, ObjPool& pool, PaletteItem& out) {
    StringView rest;
    if (paletteMode(text, rest) != PaletteMode::Actions || !startsWith(rest, StringView(u8"clone "))) {
        return false;
    }
    StringView url(rest.data() + 6, rest.length() - 6);
    while (!url.empty() && url.data()[0] == ' ') {
        url = StringView(url.data() + 1, url.length() - 1);
    }
    while (!url.empty() && (url.data()[url.length() - 1] == ' ' || url.data()[url.length() - 1] == '/')) {
        url = StringView(url.data(), url.length() - 1);
    }
    if (url.empty()) {
        return false;
    }
    // The repository's name: the last part of the URL, less ".git".
    size_t start = url.length();
    while (start > 0 && url.data()[start - 1] != '/' && url.data()[start - 1] != ':') {
        --start;
    }
    StringView name(url.data() + start, url.length() - start);
    if (name.length() > 4 && StringView(name.data() + name.length() - 4, 4) == StringView(u8".git")) {
        name = StringView(name.data(), name.length() - 4);
    }
    if (name.empty()) {
        return false;
    }
    StringBuilder directory;
    directory << cloneDirectory;
    if (!cloneDirectory.empty() && cloneDirectory.data()[cloneDirectory.length() - 1] != '/') {
        directory << StringView(u8"/");
    }
    directory << name;
    StringBuilder title;
    title << StringView(u8"git clone ") << url;
    StringBuilder subtitle;
    subtitle << StringView(u8"into ") << StringView(directory);
    out = PaletteItem();
    out.kind = PaletteKind::Action;
    out.action = PaletteAction::CloneUrl;
    out.title = pool.intern(StringView(title));
    out.subtitle = pool.intern(StringView(subtitle));
    out.command = pool.intern(url);
    out.directory = pool.intern(StringView(directory));
    out.detailLabels[0] = StringView(u8"URL");
    out.detailValues[0] = out.command;
    out.detailLabels[1] = StringView(u8"Into");
    out.detailValues[1] = out.directory;
    return true;
}

void paletteDirectoryItems(StringView typed, StringView home, ObjPool& pool, Vector<PaletteItem>& out) {
    Vector<StringView> full;
    Vector<StringView> shown;
    listMatches(typed, home, pool, full, shown);
    for (size_t at = 0; at < full.length() && at < 30; ++at) {
        PaletteItem item;
        item.kind = PaletteKind::Folder;
        item.title = shown[at];
        item.subtitle = StringView(u8"Tab completes");
        StringBuilder key;
        key << StringView(u8"path:") << full[at];
        item.key = pool.intern(StringView(key));
        item.directory = full[at];
        out.pushBack(item);
    }
}

void paletteComplete(StringView typed, StringView home, StringBuilder& out) {
    out.reset();
    ObjPool::Ref pool = ObjPool::fromMemory();
    Vector<StringView> full;
    Vector<StringView> shown;
    listMatches(typed, home, *pool, full, shown);
    if (shown.empty()) {
        return;
    }
    size_t common = shown[0].length();
    for (const StringView name : shown) {
        size_t at = 0;
        while (at < common && at < name.length() && name.data()[at] == shown[0].data()[at]) {
            ++at;
        }
        common = at;
    }
    out << StringView(shown[0].data(), common);
    if (shown.length() == 1) {
        out << StringView(u8"/");
    }
}

void paletteQuote(StringView text, StringBuilder& out) {
    out << StringView(u8"'");
    for (size_t at = 0; at < text.length(); ++at) {
        if (text.data()[at] == '\'') {
            out << StringView(u8"'\\''");
        } else {
            out << StringView(text.data() + at, 1);
        }
    }
    out << StringView(u8"'");
}

namespace {
    // `export K='v' L='w'` from "K=V" lines.
    void exports(StringView variables, StringBuilder& out) {
        out << StringView(u8"export");
        const u8* p = variables.data();
        const u8* const end = p + variables.length();
        while (p < end) {
            const u8* line = p;
            while (p < end && *p != '\n') {
                ++p;
            }
            const StringView entry(line, (size_t)(p - line));
            if (p < end) {
                ++p;
            }
            size_t equals = 0;
            while (equals < entry.length() && entry.data()[equals] != '=') {
                ++equals;
            }
            if (equals == 0 || equals >= entry.length()) {
                continue;
            }
            out << StringView(u8" ") << StringView(entry.data(), equals) << StringView(u8"=");
            paletteQuote(StringView(entry.data() + equals + 1, entry.length() - equals - 1), out);
        }
    }
}

void palettePlan(const PaletteItem& item, PaletteTarget target, StringView teleportLogin, PalettePlan& out) {
    out.command.reset();
    out.directory.reset();
    out.typed.reset();
    out.title.reset();
    out.title << item.title;
    out.directory << item.directory;
    StringBuilder run;
    switch (item.kind) {
        case PaletteKind::SshHost:
            run << StringView(u8"ssh ");
            paletteQuote(item.title, run);
            break;
        case PaletteKind::TeleportHost:
            run << StringView(u8"tsh ssh ");
            if (!teleportLogin.empty()) {
                StringBuilder target;
                target << teleportLogin << StringView(u8"@") << item.title;
                paletteQuote(StringView(target), run);
            } else {
                paletteQuote(item.title, run);
            }
            break;
        case PaletteKind::Bookmark:
        case PaletteKind::App:
            run << item.command;
            break;
        case PaletteKind::Folder:
            break;
        case PaletteKind::Env:
            exports(item.variables, run);
            if (target != PaletteTarget::CurrentTab) {
                // A new shell with the variables, then the shell itself.
                run << StringView(u8"; exec \"${SHELL:-/bin/sh}\"");
            }
            break;
        case PaletteKind::Action:
            if (item.action == PaletteAction::CloneUrl) {
                out.directory.reset();
                out.title.reset();
                size_t slash = item.directory.length();
                while (slash > 0 && item.directory.data()[slash - 1] != '/') {
                    --slash;
                }
                out.title << StringView(item.directory.data() + slash, item.directory.length() - slash);
                run << StringView(u8"git clone ");
                paletteQuote(item.command, run);
                run << StringView(u8" ");
                paletteQuote(item.directory, run);
                run << StringView(u8" && cd ");
                paletteQuote(item.directory, run);
                if (target != PaletteTarget::CurrentTab) {
                    run << StringView(u8" && exec \"${SHELL:-/bin/sh}\"");
                }
            }
            break;
    }
    if (target == PaletteTarget::CurrentTab) {
        if (item.kind == PaletteKind::Folder || (item.kind != PaletteKind::Env && item.kind != PaletteKind::Action && !item.directory.empty())) {
            out.typed << StringView(u8"cd ");
            paletteQuote(item.directory, out.typed);
            if (!run.empty()) {
                out.typed << StringView(u8" && ");
            }
        }
        out.typed << StringView(run);
        return;
    }
    out.command << StringView(run);
}

void paletteLoadRecents(StringView path, ObjPool& pool, Vector<StringView>& out) {
    out.clear();
    if (path.empty()) {
        return;
    }
    StringBuilder name;
    name << path;
    const int fd = open(name.cStr(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    Buffer text;
    char chunk[1024];
    for (;;) {
        const ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got <= 0) {
            break;
        }
        text.append(chunk, (size_t)(got));
    }
    close(fd);
    const u8* p = (const u8*)(text.data());
    const u8* const end = p + text.used();
    while (p < end && out.length() < recentsKept) {
        const u8* line = p;
        while (p < end && *p != '\n') {
            ++p;
        }
        if (p > line) {
            out.pushBack(pool.intern(StringView(line, (size_t)(p - line))));
        }
        if (p < end) {
            ++p;
        }
    }
}

void paletteRemember(StringView path, StringView key, Vector<StringView>& recents, ObjPool& pool) {
    if (key.empty() || startsWith(key, StringView(u8"action:"))) {
        return;
    }
    Vector<StringView> next;
    next.pushBack(pool.intern(key));
    for (const StringView kept : recents) {
        if (kept != key && next.length() < recentsKept) {
            next.pushBack(kept);
        }
    }
    recents.clear();
    for (const StringView kept : next) {
        recents.pushBack(kept);
    }
    if (path.empty()) {
        return;
    }
    StringBuilder text;
    for (const StringView kept : recents) {
        text << kept << StringView(u8"\n");
    }
    StringBuilder temporary;
    temporary << path << StringView(u8".tmp");
    StringBuilder final;
    final << path;
    FILE* const file = fopen(temporary.cStr(), "w");
    if (file == nullptr) {
        return;
    }
    const bool written = fwrite(text.data(), 1, text.used(), file) == text.used();
    if (fclose(file) == 0 && written) {
        rename(temporary.cStr(), final.cStr());
    } else {
        unlink(temporary.cStr());
    }
}

bool paletteEmptyHint(PaletteMode mode, StringView query, StringView configPath, StringView home, PaletteHint& out) {
    out.title.reset();
    out.lines[0].reset();
    out.lines[1].reset();
    for (StringView& line : out.example) {
        line = StringView();
    }
    if (!query.empty()) {
        out.title << StringView(u8"Nothing matches \u201c") << query << StringView(u8"\u201d");
        return true;
    }
    const auto where = [&](const char* table) {
        out.lines[0] << StringView(u8"Add ") << StringView(table) << StringView(u8" tables to");
        if (configPath.empty()) {
            out.lines[1] << StringView(u8"the config file");
        } else if (!home.empty() && configPath.length() > home.length() && StringView(configPath.data(), home.length()) == home && configPath.data()[home.length()] == '/') {
            out.lines[1] << StringView(u8"~") << StringView(configPath.data() + home.length(), configPath.length() - home.length());
        } else {
            out.lines[1] << configPath;
        }
    };
    switch (mode) {
        case PaletteMode::Apps:
            out.title << StringView(u8"No apps yet");
            where("[[app]]");
            out.example[0] = StringView(u8"[[app]]");
            out.example[1] = StringView(u8"name = \"lazygit\"");
            out.example[2] = StringView(u8"command = \"lazygit\"");
            return true;
        case PaletteMode::Env:
            out.title << StringView(u8"No environments yet");
            where("[[env]]");
            out.example[0] = StringView(u8"[[env]]");
            out.example[1] = StringView(u8"name = \"prod\"");
            out.example[2] = StringView(u8"AWS_PROFILE = \"prod\"");
            return true;
        case PaletteMode::Hosts:
            out.title << StringView(u8"No hosts");
            out.lines[0] << StringView(u8"Host entries of ~/.ssh/config,");
            out.lines[1] << StringView(u8"and Teleport nodes when tsh is on PATH");
            return true;
        case PaletteMode::Folders:
            out.title << StringView(u8"No folders yet");
            out.lines[0] << StringView(u8"Type a path: / ~/Projects");
            return true;
        case PaletteMode::Actions:
        case PaletteMode::All:
            return false;
    }
    return false;
}
