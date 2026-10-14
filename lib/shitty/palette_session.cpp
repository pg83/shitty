/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "palette_session.h"

#include "bookmark_probe.h"
#include "bookmarks.h"
#include "brand.h"
#include "composer.h"
#include "options.h"
#include "session.h"
#include "ssh_hosts.h"
#include "startup.h"

#include <lib/vterm/vterm.h>

#include <plt/platform.h>

#include <std/mem/obj_pool.h>
#include <std/str/builder.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

using namespace stl;

namespace {
    constexpr u64 teleportFresh = 60ull * 1000000ull;

    u64 monotonicNow() {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return (u64)(now.tv_sec) * 1000000ull + (u64)(now.tv_nsec) / 1000ull;
    }

    // `path` with the home directory written as ~.
    StringView shortPath(StringView path, StringView home, ObjPool& pool) {
        if (!home.empty() && path.length() >= home.length() && StringView(path.data(), home.length()) == home && (path.length() == home.length() || path.data()[home.length()] == '/')) {
            StringBuilder out;
            out << StringView(u8"~") << StringView(path.data() + home.length(), path.length() - home.length());
            return pool.intern(StringView(out));
        }
        return path;
    }

    StringView joined(ObjPool& pool, StringView a, StringView b, StringView c = StringView()) {
        StringBuilder out;
        out << a << b << c;
        return pool.intern(StringView(out));
    }

    // Where tsh is, when it is on PATH at all.
    bool findOnPath(const char* name, StringBuilder& out) {
        const char* const path = getenv("PATH");
        if (path == nullptr) {
            return false;
        }
        const char* p = path;
        for (;;) {
            const char* end = p;
            while (*end != '\0' && *end != ':') {
                ++end;
            }
            if (end > p) {
                out.reset();
                out << StringView((const u8*)(p), (size_t)(end - p)) << StringView(u8"/") << StringView(name);
                if (access(out.cStr(), X_OK) == 0) {
                    return true;
                }
            }
            if (*end == '\0') {
                return false;
            }
            p = end + 1;
        }
    }
}

PaletteSession::PaletteSession(Composer& composer_, PaletteHost& host_)
    : composer(composer_)
    , host(host_)
{
    teleportWaiter.callback = this;
}

PaletteSession::~PaletteSession() noexcept {
    if (teleportFd >= 0) {
        composer.platform->poller()->cancel(teleportWaiter);
        ::close(teleportFd);
    }
    delete pool;
}

void PaletteSession::open() {
    delete pool;
    pool = ObjPool::fromMemoryRaw();
    homeDirectory(home);
    // The recents live beside the bookmarks.
    recentsPath.reset();
    StringView beside = composer.bookmarks != nullptr ? composer.bookmarks->path : StringView();
    size_t slash = beside.length();
    while (slash > 0 && beside.data()[slash - 1] != '/') {
        --slash;
    }
    if (slash > 0) {
        recentsPath.append(beside.data(), slash);
        recentsPath.append("palette_recent", 14);
    }
    paletteLoadRecents(StringView(recentsPath), *pool, recents);
    collect();
    startTeleport();
    shown_ = true;
    field.begin(StringView());
    textChanged();
}

void PaletteSession::close() {
    shown_ = false;
}

void PaletteSession::collect() {
    base.clear();
    ObjPool& p = *pool;
    const StringView homeView(home);
    if (composer.bookmarks != nullptr) {
        for (const Bookmark& bookmark : composer.bookmarks->items) {
            PaletteItem item;
            item.kind = PaletteKind::Bookmark;
            item.title = bookmark.title;
            item.subtitle = !bookmark.command.empty() ? bookmark.command : shortPath(bookmark.directory, homeView, p);
            item.star = true;
            item.bookmark = bookmark.id;
            item.key = joined(p, StringView(u8"bm:"), bookmark.title);
            item.command = bookmark.command;
            item.directory = bookmark.directory;
            item.detailLabels[0] = StringView(u8"Runs");
            item.detailValues[0] = bookmark.command.empty() ? StringView(u8"the shell") : bookmark.command;
            item.detailLabels[1] = StringView(u8"In");
            item.detailValues[1] = bookmark.directory.empty() ? StringView(u8"—") : bookmark.directory;
            item.detailLabels[2] = StringView(u8"Folder");
            item.detailValues[2] = bookmark.folder.empty() ? StringView(u8"—") : bookmark.folder;
            base.pushBack(item);
        }
    }
    Vector<SshHost> hosts;
    sshConfigHostsFromHome(homeView, p, hosts);
    for (const SshHost& host : hosts) {
        PaletteItem item;
        item.kind = PaletteKind::SshHost;
        item.title = host.alias;
        StringBuilder where;
        if (!host.user.empty()) {
            where << host.user << StringView(u8"@");
        }
        where << (host.hostName.empty() ? host.alias : host.hostName);
        if (!host.port.empty()) {
            where << StringView(u8":") << host.port;
        }
        item.subtitle = p.intern(StringView(where));
        item.badge = StringView(u8"ssh config");
        item.key = joined(p, StringView(u8"ssh:"), host.alias);
        item.detailLabels[0] = StringView(u8"Host");
        item.detailValues[0] = host.hostName.empty() ? host.alias : host.hostName;
        item.detailLabels[1] = StringView(u8"User");
        item.detailValues[1] = host.user.empty() ? StringView(u8"—") : host.user;
        item.detailLabels[2] = StringView(u8"Port");
        item.detailValues[2] = host.port.empty() ? StringView(u8"22") : host.port;
        base.pushBack(item);
    }
    Vector<TeleportHost> nodes;
    teleportHosts(StringView(teleportJson), p, nodes);
    for (const TeleportHost& node : nodes) {
        PaletteItem item;
        item.kind = PaletteKind::TeleportHost;
        item.title = node.hostname;
        item.subtitle = node.labels.empty() ? node.address : node.labels;
        item.badge = StringView(u8"Teleport");
        item.key = joined(p, StringView(u8"tsh:"), node.hostname);
        item.detailLabels[0] = StringView(u8"Address");
        item.detailValues[0] = node.address.empty() ? StringView(u8"—") : node.address;
        item.detailLabels[1] = StringView(u8"Labels");
        item.detailValues[1] = node.labels.empty() ? StringView(u8"—") : node.labels;
        item.detailLabels[2] = StringView(u8"Login");
        item.detailValues[2] = composer.opts->teleportLogin.empty() ? StringView(u8"tsh's own") : composer.opts->teleportLogin;
        base.pushBack(item);
    }
    // Folders opened lately.
    for (const StringView key : recents) {
        if (key.length() > 4 && StringView(key.data(), 4) == StringView(u8"dir:")) {
            PaletteItem item;
            item.kind = PaletteKind::Folder;
            item.directory = StringView(key.data() + 4, key.length() - 4);
            item.title = shortPath(item.directory, homeView, p);
            item.subtitle = StringView(u8"recent");
            item.key = key;
            item.detailLabels[0] = StringView(u8"Path");
            item.detailValues[0] = item.directory;
            base.pushBack(item);
        }
    }
    for (const PaletteApp& app : composer.opts->paletteApps) {
        PaletteItem item;
        item.kind = PaletteKind::App;
        item.title = app.name;
        item.subtitle = app.command;
        item.key = joined(p, StringView(u8"app:"), app.name);
        item.command = app.command;
        item.directory = app.directory;
        item.detailLabels[0] = StringView(u8"Runs");
        item.detailValues[0] = app.command;
        item.detailLabels[1] = StringView(u8"In");
        item.detailValues[1] = app.directory.empty() ? StringView(u8"where the tab is") : app.directory;
        base.pushBack(item);
    }
    for (const PaletteEnv& env : composer.opts->paletteEnvs) {
        PaletteItem item;
        item.kind = PaletteKind::Env;
        item.title = env.name;
        StringBuilder shown;
        for (size_t at = 0; at < env.variables.length(); ++at) {
            const u8 c = env.variables.data()[at];
            if (c == '\n') {
                if (at + 1 < env.variables.length()) {
                    shown << StringView(u8" · ");
                }
            } else {
                shown << StringView(&c, 1);
            }
        }
        item.subtitle = p.intern(StringView(shown));
        item.variables = env.variables;
        item.badge = StringView(currentEnv) == env.name ? StringView(u8"current") : StringView();
        item.key = joined(p, StringView(u8"env:"), env.name);
        item.detailLabels[0] = StringView(u8"Sets");
        item.detailValues[0] = item.subtitle;
        base.pushBack(item);
    }
    paletteActions(p, base);
}

void PaletteSession::textChanged() {
    items.clear();
    for (const PaletteItem& made : base) {
        items.pushBack(made);
    }
    const StringView text = field.text();
    StringView rest;
    const PaletteMode current = paletteMode(text, rest);
    if (current == PaletteMode::Folders && !rest.empty() && (rest.data()[0] == '/' || rest.data()[0] == '~')) {
        paletteDirectoryItems(rest, StringView(home), *pool, items);
    }
    const StringView clones = composer.opts->cloneDirectory;
    StringBuilder expanded;
    if (!clones.empty() && clones.data()[0] == '~') {
        expanded << StringView(home) << StringView(clones.data() + 1, clones.length() - 1);
    } else {
        expanded << clones;
    }
    PaletteItem clone;
    if (paletteCloneItem(text, StringView(expanded), *pool, clone)) {
        items.pushBack(clone);
    }
    paletteQuery(items, text, recents, rows_);
    selected_ = paletteFirstItem(rows_);
    host.paletteChanged();
}

PaletteMode PaletteSession::mode() {
    StringView rest;
    return paletteMode(field.text(), rest);
}

bool PaletteSession::hint(PaletteHint& out) {
    if (!rows_.empty()) {
        return false;
    }
    StringView rest;
    const PaletteMode current = paletteMode(field.text(), rest);
    return paletteEmptyHint(current, rest, composer.opts->configPath, StringView(home), out);
}

const PaletteItem* PaletteSession::item(size_t row) const {
    if (row >= rows_.length() || rows_[row].heading || rows_[row].item >= items.length()) {
        return nullptr;
    }
    return &items[rows_[row].item];
}

void PaletteSession::move(int step) {
    selected_ = paletteStep(rows_, selected_, step);
    host.paletteChanged();
}

void PaletteSession::select(size_t row) {
    if (item(row) != nullptr) {
        selected_ = row;
        host.paletteChanged();
    }
}

void PaletteSession::setText(StringView text) {
    field.begin(text);
    // The caret at the end, nothing selected: more is typed after it.
    plt::KeyInput end;
    end.key = plt::InputKey::End;
    field.key(end);
    textChanged();
}

bool PaletteSession::complete() {
    const StringView text = field.text();
    StringView rest;
    const PaletteMode current = paletteMode(text, rest);
    StringBuilder prefix;
    prefix << StringView(text.data(), text.length() - rest.length());
    if (current == PaletteMode::Folders && !rest.empty() && (rest.data()[0] == '/' || rest.data()[0] == '~')) {
        StringBuilder completed;
        paletteComplete(rest, StringView(home), completed);
        if (completed.empty() || StringView(completed) == rest) {
            return false;
        }
        StringBuilder next;
        next << StringView(prefix) << StringView(completed);
        setText(StringView(next));
        return true;
    }
    const PaletteItem* const picked = item(selected_);
    if (picked == nullptr || picked->kind == PaletteKind::Action) {
        return false;
    }
    StringBuilder next;
    next << StringView(prefix) << picked->title;
    setText(StringView(next));
    return true;
}

void PaletteSession::remember(const PaletteItem& picked) {
    StringView key = picked.key;
    StringBuilder dir;
    if (picked.kind == PaletteKind::Folder) {
        // A folder is remembered by where it is, whichever way it was found.
        dir << StringView(u8"dir:") << picked.directory;
        key = StringView(dir);
    }
    paletteRemember(StringView(recentsPath), key, recents, *pool);
}

bool PaletteSession::pick(PaletteTarget target) {
    const PaletteItem* const found = item(selected_);
    SessionSet* const sessions = composer.sessions;
    if (found == nullptr || sessions == nullptr) {
        return false;
    }
    const PaletteItem picked = *found;
    if (picked.kind == PaletteKind::Action) {
        switch (picked.action) {
            case PaletteAction::NewTab:
                sessions->newSession();
                return true;
            case PaletteAction::OpenFolder:
                setText(StringView(u8"/ ~/"));
                return false;
            case PaletteAction::Clone:
                setText(StringView(u8"> clone "));
                return false;
            case PaletteAction::CloneUrl:
            case PaletteAction::None:
                break;
        }
    }
    remember(picked);
    if (picked.kind == PaletteKind::Env) {
        currentEnv.reset();
        currentEnv.append(picked.title.data(), picked.title.length());
    }
    if (picked.kind == PaletteKind::Bookmark && target == PaletteTarget::NewTab && composer.bookmarks != nullptr) {
        // A bookmark opens as itself: the sidebar's row, its reconnect.
        if (const Bookmark* const bookmark = composer.bookmarks->find(picked.bookmark)) {
            sessions->openBookmark(*bookmark);
            return true;
        }
    }
    PalettePlan plan;
    PaletteTarget where = target;
    if (where == PaletteTarget::CurrentTab) {
        // Typed only into a shell that waits at its prompt: with a program
        // in front, the line would be that program's input.
        const size_t active = sessions->activeIndex();
        const u64 pane = active < sessions->count() ? sessions->focusedPane(active) : 0;
        Vterm* const terminal = sessions->activeTerminal();
        if (pane == 0 || terminal == nullptr || sessions->paneExited(pane) || sessions->paneForeground(pane) != sessions->panePid(pane)) {
            where = PaletteTarget::NewTab;
        } else {
            palettePlan(picked, where, composer.opts->teleportLogin, plan);
            StringBuilder line;
            line << StringView(plan.typed) << StringView(u8"\r");
            terminal->sendBytes(StringView(line), true);
            return true;
        }
    }
    palettePlan(picked, where, composer.opts->teleportLogin, plan);
    // Hosts and clones name their tab; a folder or an app leaves it to
    // the program.
    const bool named = picked.kind == PaletteKind::SshHost || picked.kind == PaletteKind::TeleportHost || picked.kind == PaletteKind::Bookmark || picked.action == PaletteAction::CloneUrl;
    const StringView title = named ? StringView(plan.title) : StringView();
    sessions->openCommand(StringView(plan.command), StringView(plan.directory), title);
    return true;
}

bool PaletteSession::bookmarkPicked() {
    const PaletteItem* const found = item(selected_);
    BookmarkShelf* const shelf = composer.bookmarks;
    if (found == nullptr || shelf == nullptr) {
        return false;
    }
    Bookmark draft;
    PalettePlan plan;
    switch (found->kind) {
        case PaletteKind::SshHost:
        case PaletteKind::TeleportHost:
            palettePlan(*found, PaletteTarget::NewTab, composer.opts->teleportLogin, plan);
            draft.title = found->title;
            draft.command = pool->intern(StringView(plan.command));
            break;
        case PaletteKind::Folder: {
            size_t slash = found->directory.length();
            while (slash > 1 && found->directory.data()[slash - 1] == '/') {
                --slash;
            }
            size_t start = slash;
            while (start > 0 && found->directory.data()[start - 1] != '/') {
                --start;
            }
            draft.title = pool->intern(StringView(found->directory.data() + start, slash - start));
            draft.directory = found->directory;
            break;
        }
        default:
            return false;
    }
    u64 id = 0;
    if (!pinBookmark(*shelf, *pool, composer.brand != nullptr ? composer.brand->identifier() : StringView(), draft, id)) {
        return false;
    }
    if (composer.bookmarkProbe != nullptr) {
        composer.bookmarkProbe->watch(*shelf);
    }
    collect();
    textChanged();
    return true;
}

void PaletteSession::startTeleport() {
    if (teleportFd >= 0 || (teleportAt != 0 && monotonicNow() - teleportAt < teleportFresh)) {
        return;
    }
    StringBuilder tsh;
    if (!findOnPath("tsh", tsh)) {
        return;
    }
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        return;
    }
    fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC);
    char* const argv[] = {tsh.cStr(), const_cast<char*>("ls"), const_cast<char*>("--format=json"), nullptr};
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return;
    }
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        execv(argv[0], argv);
        _exit(127);
    }
    ::close(pipeFds[1]);
    fcntl(pipeFds[0], F_SETFL, fcntl(pipeFds[0], F_GETFL) | O_NONBLOCK);
    teleportFd = pipeFds[0];
    teleportReading.reset();
    teleportWaiter.fd = {.fd = teleportFd, .flags = PollFlag::In};
    composer.platform->poller()->arm(teleportWaiter);
}

void PaletteSession::ready(PollFD) {
    char chunk[4096];
    for (;;) {
        const ssize_t got = read(teleportFd, chunk, sizeof(chunk));
        if (got > 0) {
            teleportReading.append(chunk, (size_t)(got));
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
            composer.platform->poller()->arm(teleportWaiter);
            return;
        }
        break;
    }
    // EOF: tsh is done. Its exit status is the SIGCHLD handler's, so the
    // output is the answer - a list, or anything else, which lists nothing.
    ::close(teleportFd);
    teleportFd = -1;
    teleportJson.reset();
    teleportJson.append(teleportReading.data(), teleportReading.used());
    teleportAt = monotonicNow();
    if (shown_) {
        collect();
        textChanged();
    }
}
