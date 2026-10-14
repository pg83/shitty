/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "bookmarks.h"

#include "toml.h"

#include <std/ios/fs_utils.h>
#include <std/ios/sys.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/sys/fd.h>
#include <std/sys/throw.h>

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace stl;

bool defaultBookmarksPath(StringView configPath, StringBuilder& out) {
    if (configPath.empty()) {
        return false;
    }
    size_t afterSlash = 0;
    for (size_t at = 0; at < configPath.length(); ++at) {
        if (configPath[at] == '/') {
            afterSlash = at + 1;
        }
    }
    out << StringView(configPath.data(), afterSlash) << StringView(u8"bookmarks.toml");
    return true;
}

namespace {
    struct BookmarkSink final: public TomlSink {
        enum class Key : u8 {
            None,
            Title,
            Command,
            Directory,
            Folder,
            Name,
            Icon,
        };

        StringView identifier;
        StringView path;
        ObjPool& pool;
        u64& nextId;
        Vector<Bookmark>& out;
        Vector<FolderStyle>* folders;
        Bookmark entry;
        // Inside a [[folder]] table instead, and what it has said so far.
        bool folderOpen = false;
        FolderStyle style;
        u32 folderBlocks = 0;
        Key pending = Key::None;
        // Inside a [[bookmark]] table, as opposed to before the first one
        // or inside some other table.
        bool open = false;
        bool broken = false;
        // Nested values have no place in a bookmark; their scalars are
        // skipped rather than mistaken for the key that opened them.
        int depth = 0;
        // [[bookmark]] headers seen so far, understood or not.
        u32 blocks = 0;

        BookmarkSink(StringView identifier_, StringView path_, ObjPool& pool_, u64& nextId_, Vector<Bookmark>& out_, Vector<FolderStyle>* folders_)
            : identifier(identifier_)
            , path(path_)
            , pool(pool_)
            , nextId(nextId_)
            , out(out_)
            , folders(folders_)
        {
        }

        void warn(const char* what, StringView name) {
            if (identifier.empty()) {
                return;
            }
            sysE << identifier << StringView(u8": ") << path << StringView(u8": ") << StringView(what);
            if (!name.empty()) {
                sysE << StringView(u8": ") << name;
            }
            sysE << endL;
        }

        void finish() {
            if (folderOpen) {
                folderOpen = false;
                const FolderStyle done = style;
                const bool wasBroken = broken;
                style = FolderStyle();
                broken = false;
                pending = Key::None;
                if (wasBroken) {
                    return;
                }
                if (done.name.empty()) {
                    warn("folder without a name", StringView());
                    return;
                }
                if (folders != nullptr) {
                    folders->pushBack(done);
                }
                return;
            }
            if (!open) {
                return;
            }
            Bookmark done = entry;
            const bool wasBroken = broken;
            entry = Bookmark();
            broken = false;
            pending = Key::None;
            if (wasBroken) {
                // The offending key warned already; half a bookmark
                // would open something the user did not write.
                return;
            }
            if (done.command.empty() && done.directory.empty()) {
                warn("bookmark without a command or a dir", done.title);
                return;
            }
            if (done.title.empty()) {
                done.title = done.command.empty() ? done.directory : done.command;
            }
            done.id = nextId++;
            out.pushBack(done);
        }

        bool tomlTable(const StringView* segments, size_t count, bool array) override {
            finish();
            open = false;
            if (count == 1 && segments[0] == StringView(u8"bookmark")) {
                if (array) {
                    open = true;
                    entry.block = blocks++;
                } else {
                    warn("bookmark is an array of tables, write [[bookmark]]", StringView());
                }
                return true;
            }
            if (count == 1 && array && segments[0] == StringView(u8"folder")) {
                folderOpen = true;
                style.block = folderBlocks++;
                return true;
            }
            warn("only [[bookmark]] and [[folder]] tables belong in this file, ignoring", count != 0 ? segments[0] : StringView());
            return true;
        }

        bool tomlKey(const StringView* segments, size_t count) override {
            pending = Key::None;
            if (depth != 0) {
                return true;
            }
            const StringView name = count != 0 ? segments[count - 1] : StringView();
            if (folderOpen) {
                if (count == 1 && name == StringView(u8"name")) {
                    pending = Key::Name;
                } else if (count == 1 && name == StringView(u8"icon")) {
                    pending = Key::Icon;
                } else {
                    warn("unknown folder key, ignoring", name);
                }
                return true;
            }
            if (!open) {
                warn("key outside a [[bookmark]] table, ignoring", name);
                return true;
            }
            if (count == 1 && name == StringView(u8"title")) {
                pending = Key::Title;
            } else if (count == 1 && name == StringView(u8"command")) {
                pending = Key::Command;
            } else if (count == 1 && name == StringView(u8"dir")) {
                pending = Key::Directory;
            } else if (count == 1 && name == StringView(u8"folder")) {
                pending = Key::Folder;
            } else {
                warn("unknown bookmark key, ignoring", name);
            }
            return true;
        }

        bool tomlScalar(TomlType type, StringView text) override {
            const Key key = pending;
            pending = Key::None;
            if (depth != 0 || key == Key::None) {
                return true;
            }
            if (type != TomlType::String) {
                warn("bookmark value is not a string", text);
                broken = true;
                return true;
            }
            const StringView value = pool.intern(text);
            if (key == Key::Name) {
                style.name = value;
            } else if (key == Key::Icon) {
                style.icon = value;
            } else if (key == Key::Title) {
                entry.title = value;
            } else if (key == Key::Command) {
                entry.command = value;
            } else if (key == Key::Folder) {
                entry.folder = value;
            } else {
                entry.directory = value;
            }
            return true;
        }

        bool nestedBegin() {
            if (depth == 0 && pending != Key::None) {
                warn("bookmark value is not a string", StringView());
                broken = true;
            }
            pending = Key::None;
            ++depth;
            return true;
        }

        bool nestedEnd() {
            if (depth != 0) {
                --depth;
            }
            return true;
        }

        bool tomlArrayBegin() override {
            return nestedBegin();
        }

        bool tomlArrayEnd() override {
            return nestedEnd();
        }

        bool tomlInlineTableBegin() override {
            return nestedBegin();
        }

        bool tomlInlineTableEnd() override {
            return nestedEnd();
        }

        void tomlError(size_t line, StringView message) override {
            if (!identifier.empty()) {
                sysE << identifier << StringView(u8": ") << path << StringView(u8":") << line << StringView(u8": ") << message << StringView(u8"; ignoring the rest of the file") << endL;
            }
            // What came before the error stands; the entry it cut short
            // does not.
            open = false;
            folderOpen = false;
            entry = Bookmark();
        }
    };

    u32 appendString(Buffer& storage, StringView text) {
        const u32 offset = (u32)(storage.used());
        storage.append(text.data(), text.length());
        storage.append("", 1);
        return offset;
    }
}

void parseBookmarks(StringView text, StringView identifier, StringView path, ObjPool& pool, u64& nextId, Vector<Bookmark>& out, Vector<FolderStyle>* folders) {
    BookmarkSink sink(identifier, path, pool, nextId, out, folders);
    parseToml(text, sink);
    sink.finish();
}

void loadBookmarks(StringView path, StringView identifier, ObjPool& pool, u64& nextId, Vector<Bookmark>& out, Vector<FolderStyle>* folders) {
    if (path.empty()) {
        return;
    }
    Buffer pathBuf{path};
    Buffer text;
    try {
        readFileContent(pathBuf, text);
    } catch (Exception&) {
        return;
    }
    parseBookmarks(StringView(text), identifier, path, pool, nextId, out, folders);
}

LaunchCommand bookmarkLaunchCommand(const LaunchCommand& shell, StringView command) {
    LaunchCommand launch;
    launch.executableOffset = appendString(launch.storage, StringView(shell.executable()));
    for (size_t at = 0; at < shell.offsets.length(); ++at) {
        launch.offsets.pushBack(appendString(launch.storage, StringView(shell.argument(at))));
    }
    if (!command.empty()) {
        launch.offsets.pushBack(appendString(launch.storage, StringView(u8"-c")));
        launch.offsets.pushBack(appendString(launch.storage, command));
    }
    return launch;
}

const Bookmark* BookmarkShelf::find(u64 id) const {
    const size_t at = indexOf(id);
    return at < items.length() ? &items[at] : nullptr;
}

const FolderStyle* BookmarkShelf::style(StringView folder) const {
    for (const FolderStyle& entry : folders) {
        if (entry.name == folder) {
            return &entry;
        }
    }
    return nullptr;
}

size_t BookmarkShelf::indexOf(u64 id) const {
    for (size_t at = 0; at < items.length(); ++at) {
        if (items[at].id == id) {
            return at;
        }
    }
    return items.length();
}

void bookmarkStatus(const Bookmark& bookmark, BookmarkState state, StringBuilder& out) {
    out.reset();
    const StringView what = bookmark.command.empty() ? bookmark.directory : bookmark.command;
    switch (state) {
        case BookmarkState::Open:
            out << StringView(u8"open · ") << what;
            break;
        case BookmarkState::Closed:
            out << StringView(u8"not open · ") << what;
            break;
        case BookmarkState::Exited:
            out << StringView(u8"exited · click to reconnect");
            break;
        case BookmarkState::Unreachable:
            out << StringView(u8"unreachable · ") << what;
            break;
    }
}

bool sameBookmark(const Bookmark& a, const Bookmark& b) {
    return a.title == b.title && a.command == b.command && a.directory == b.directory;
}

namespace {
    void tomlString(StringView text, StringBuilder& out) {
        static const char hex[] = "0123456789ABCDEF";
        out << StringView(u8"\"");
        for (size_t at = 0; at < text.length(); ++at) {
            const u8 byte = (u8)(text[at]);
            if (byte == '"' || byte == '\\') {
                const u8 escaped[2] = {(u8)(0x5C), byte};
                out << StringView(escaped, 2);
            } else if (byte < 0x20 || byte == 0x7F) {
                const u8 escaped[6] = {'\\', 'u', '0', '0', (u8)(hex[byte >> 4]), (u8)(hex[byte & 15])};
                out << StringView(escaped, 6);
            } else {
                out << StringView(text.data() + at, 1);
            }
        }
        out << StringView(u8"\"");
    }

    void keyLine(const char* key, StringView value, StringBuilder& out) {
        if (value.empty()) {
            return;
        }
        out << StringView(key) << StringView(u8" = ");
        tomlString(value, out);
        out << StringView(u8"\n");
    }

    // A line's text without its line break and surrounding blanks.
    StringView trimmed(StringView line) {
        if (!line.empty() && line[line.length() - 1] == '\n') {
            line = StringView(line.data(), line.length() - 1);
        }
        return line.stripCr().stripSpace();
    }

    bool isHeader(StringView line) {
        const StringView text = trimmed(line);
        return !text.empty() && text[0] == '[';
    }

    bool isArrayHeader(StringView line, StringView table) {
        StringView text = trimmed(line);
        if (!text.startsWith(StringView(u8"[["))) {
            return false;
        }
        text = StringView(text.data() + 2, text.length() - 2);
        size_t close = 0;
        while (close + 1 < text.length() && !(text[close] == ']' && text[close + 1] == ']')) {
            ++close;
        }
        if (close + 1 >= text.length()) {
            return false;
        }
        return StringView(text.data(), close).stripSpace() == table;
    }

    bool isQuiet(StringView line) {
        const StringView text = trimmed(line);
        return text.empty() || text[0] == '#';
    }

    bool writeWhole(StringView path, StringView content) {
        Buffer pathBuf{path};
        // The directory may not be there yet - a user who never wrote a
        // config has no config directory - and one level is all a
        // default path ever needs.
        size_t slash = path.length();
        while (slash > 0 && path[slash - 1] != '/') {
            --slash;
        }
        if (slash > 1) {
            Buffer directory{StringView(path.data(), slash - 1)};
            if (::mkdir(directory.cStr(), 0755) < 0 && errno != EEXIST) {
                return false;
            }
        }
        StringBuilder tmpPath;
        tmpPath << path << StringView(u8".tmp.") << (i64)(getpid());
        Buffer tmpPathBuf{StringView(tmpPath)};
        try {
            const int rawFd = ::open(tmpPathBuf.cStr(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (rawFd < 0) {
                Errno().raise(StringBuilder() << StringView(u8"open() failed"));
            }
            ScopedFD fd(rawFd);
            fd.write(content.data(), content.length());
            fd.fsync();
            fd.close();
        } catch (Exception&) {
            ::unlink(tmpPathBuf.cStr());
            return false;
        }
        if (::rename(tmpPathBuf.cStr(), pathBuf.cStr()) < 0) {
            ::unlink(tmpPathBuf.cStr());
            return false;
        }
        return true;
    }

    // The file as it is now; empty when there is none yet.
    void readWhole(StringView path, Buffer& out) {
        out.reset();
        Buffer pathBuf{path};
        try {
            readFileContent(pathBuf, out);
        } catch (Exception&) {
            out.reset();
        }
    }
}

void bookmarkBlock(const Bookmark& bookmark, StringBuilder& out) {
    out << StringView(u8"[[bookmark]]\n");
    keyLine("title", bookmark.title, out);
    keyLine("command", bookmark.command, out);
    keyLine("dir", bookmark.directory, out);
    keyLine("folder", bookmark.folder, out);
}

void appendBookmark(StringView text, const Bookmark& bookmark, StringBuilder& out) {
    out.reset();
    out << text;
    if (!text.empty()) {
        if (text[text.length() - 1] != '\n') {
            out << StringView(u8"\n");
        }
        out << StringView(u8"\n");
    }
    bookmarkBlock(bookmark, out);
}

// The one cut both of these make: the entry's block out, and `replacement`'s
// block, when there is one, in its place.
static bool spliceBookmark(StringView text, const Bookmark& bookmark, const Bookmark* replacement, StringBuilder& out);

bool removeBookmark(StringView text, const Bookmark& bookmark, StringBuilder& out) {
    return spliceBookmark(text, bookmark, nullptr, out);
}

bool replaceBookmark(StringView text, const Bookmark& bookmark, const Bookmark& replacement, StringBuilder& out) {
    return spliceBookmark(text, bookmark, &replacement, out);
}

static bool spliceTable(StringView text, StringView table, u32 ordinal, const StringView* replacement, StringBuilder& out);

static bool spliceBookmark(StringView text, const Bookmark& bookmark, const Bookmark* replacement, StringBuilder& out) {
    ObjPool::Ref pool = ObjPool::fromMemory();
    Vector<Bookmark> entries;
    u64 nextId = 1;
    // Quietly: the file was warned about when it was loaded.
    parseBookmarks(text, StringView(), StringView(), *pool, nextId, entries);
    const Bookmark* found = nullptr;
    for (const Bookmark& entry : entries) {
        if (sameBookmark(entry, bookmark)) {
            found = &entry;
            break;
        }
    }
    if (found == nullptr) {
        return false;
    }
    StringBuilder block;
    if (replacement != nullptr) {
        bookmarkBlock(*replacement, block);
    }
    const StringView blockText(block);
    return spliceTable(text, StringView(u8"bookmark"), found->block, replacement != nullptr ? &blockText : nullptr, out);
}

// The one cut: the `ordinal`-th [[table]] block out of the text, and the
// replacement in its place when there is one.
static bool spliceTable(StringView text, StringView table, u32 ordinal, const StringView* replacement, StringBuilder& out) {
    // Line by line: where the entry's header starts, and where the next
    // header does.
    size_t start = text.length();
    size_t end = text.length();
    u32 seen = 0;
    bool inside = false;
    // The run of blank and comment lines just above the next header, and
    // the first comment in it. A comment there introduces what follows and
    // stays; the blank lines between the two blocks go with a removed one,
    // so no gap is left where it was, and stay beside a replaced one, which
    // keeps the gap it had.
    size_t quietFrom = text.length();
    size_t commentFrom = text.length();
    for (size_t at = 0; at < text.length();) {
        size_t next = at;
        while (next < text.length() && text[next] != '\n') {
            ++next;
        }
        if (next < text.length()) {
            ++next;
        }
        const StringView line(text.data() + at, next - at);
        if (inside) {
            if (isHeader(line)) {
                if (replacement != nullptr) {
                    end = quietFrom < at ? quietFrom : at;
                } else {
                    end = commentFrom < at ? commentFrom : at;
                }
                break;
            }
            if (isQuiet(line)) {
                if (quietFrom == text.length()) {
                    quietFrom = at;
                }
                if (commentFrom == text.length() && !trimmed(line).empty()) {
                    commentFrom = at;
                }
            } else {
                quietFrom = text.length();
                commentFrom = text.length();
            }
        } else if (isArrayHeader(line, table)) {
            if (seen == ordinal) {
                start = at;
                inside = true;
            }
            ++seen;
        }
        at = next;
    }
    if (!inside) {
        return false;
    }
    if (replacement == nullptr && end == text.length()) {
        // The last block going: the blank lines that parted it from the one
        // before go too, so the file does not end in a gap.
        while (start > 0 && text[start - 1] == '\n' && (start == 1 || text[start - 2] == '\n')) {
            --start;
        }
    }
    out.reset();
    out << StringView(text.data(), start);
    if (replacement != nullptr) {
        out << *replacement;
    }
    out << StringView(text.data() + end, text.length() - end);
    return true;
}

void shellCommandLine(StringView arguments, StringBuilder& out) {
    out.reset();
    bool first = true;
    for (size_t at = 0; at < arguments.length();) {
        size_t stop = at;
        while (stop < arguments.length() && arguments[stop] != '\0') {
            ++stop;
        }
        const StringView argument(arguments.data() + at, stop - at);
        if (!first) {
            out << StringView(u8" ");
        }
        first = false;
        bool plain = !argument.empty();
        for (size_t index = 0; index < argument.length() && plain; ++index) {
            const char c = argument[index];
            plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/' || c == ':' || c == '@' || c == '%' || c == '+' || c == '=' || c == ',';
        }
        if (plain) {
            out << argument;
        } else {
            out << StringView(u8"'");
            for (size_t index = 0; index < argument.length(); ++index) {
                if (argument[index] == '\'') {
                    out << StringView(u8"'\\''");
                } else {
                    out << StringView(argument.data() + index, 1);
                }
            }
            out << StringView(u8"'");
        }
        at = stop + 1;
    }
}

void reloadBookmarks(BookmarkShelf& shelf, ObjPool& pool, StringView identifier) {
    Vector<Bookmark> fresh;
    u64 unused = 1;
    shelf.folders.clear();
    loadBookmarks(shelf.path, identifier, pool, unused, fresh, &shelf.folders);
    Vector<bool> taken;
    for (size_t at = 0; at < shelf.items.length(); ++at) {
        taken.pushBack(false);
    }
    for (size_t at = 0; at < fresh.length(); ++at) {
        Bookmark& entry = fresh.mut(at);
        entry.id = 0;
        for (size_t old = 0; old < shelf.items.length(); ++old) {
            if (!taken[old] && sameBookmark(shelf.items[old], entry)) {
                taken.mut(old) = true;
                entry.id = shelf.items[old].id;
                break;
            }
        }
        if (entry.id == 0) {
            entry.id = shelf.nextId++;
        }
    }
    shelf.items.clear();
    for (const Bookmark& entry : fresh) {
        shelf.items.pushBack(entry);
    }
}

bool pinBookmark(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, const Bookmark& bookmark, u64& id) {
    if (shelf.path.empty()) {
        return false;
    }
    Buffer text;
    readWhole(shelf.path, text);
    StringBuilder next;
    appendBookmark(StringView(text), bookmark, next);
    if (!writeWhole(shelf.path, StringView(next))) {
        return false;
    }
    // The ids the shelf already had, so the new entry is the one that
    // comes back without one of them.
    Vector<u64> before;
    for (const Bookmark& entry : shelf.items) {
        before.pushBack(entry.id);
    }
    reloadBookmarks(shelf, pool, identifier);
    id = 0;
    for (const Bookmark& entry : shelf.items) {
        bool old = false;
        for (u64 was : before) {
            old = old || was == entry.id;
        }
        if (!old && sameBookmark(entry, bookmark)) {
            id = entry.id;
        }
    }
    return id != 0;
}

bool unpinBookmark(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, u64 id) {
    const Bookmark* const bookmark = shelf.find(id);
    if (bookmark == nullptr || shelf.path.empty()) {
        return false;
    }
    Buffer text;
    readWhole(shelf.path, text);
    StringBuilder next;
    if (!removeBookmark(StringView(text), *bookmark, next)) {
        return false;
    }
    if (!writeWhole(shelf.path, StringView(next))) {
        return false;
    }
    reloadBookmarks(shelf, pool, identifier);
    return true;
}

bool setBookmarkFolder(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, u64 id, StringView folder) {
    const Bookmark* const bookmark = shelf.find(id);
    if (bookmark == nullptr || shelf.path.empty()) {
        return false;
    }
    Bookmark moved = *bookmark;
    moved.folder = folder;
    Buffer text;
    readWhole(shelf.path, text);
    StringBuilder next;
    if (!replaceBookmark(StringView(text), *bookmark, moved, next)) {
        return false;
    }
    if (!writeWhole(shelf.path, StringView(next))) {
        return false;
    }
    reloadBookmarks(shelf, pool, identifier);
    return true;
}

size_t folderIndex(const Vector<StringView>& order, StringView folder) {
    for (size_t at = 0; at < order.length(); ++at) {
        if (order[at] == folder) {
            return at;
        }
    }
    return order.length();
}

void folderOrder(const BookmarkShelf* shelf, const Vector<StringView>& windowFolders, Vector<StringView>& out) {
    out.clear();
    if (shelf != nullptr) {
        for (const Bookmark& bookmark : shelf->items) {
            if (!bookmark.folder.empty() && folderIndex(out, bookmark.folder) == out.length()) {
                out.pushBack(bookmark.folder);
            }
        }
        // A folder saved only for its look still stands.
        for (const FolderStyle& style : shelf->folders) {
            if (!style.name.empty() && folderIndex(out, style.name) == out.length()) {
                out.pushBack(style.name);
            }
        }
    }
    for (const StringView folder : windowFolders) {
        if (!folder.empty() && folderIndex(out, folder) == out.length()) {
            out.pushBack(folder);
        }
    }
}

namespace {
    void folderBlock(StringView name, StringView icon, StringBuilder& out) {
        out << StringView(u8"[[folder]]\n");
        keyLine("name", name, out);
        keyLine("icon", icon, out);
    }
}

namespace {
    // The [[folder]] table of `folder` rewritten: to `icon` (empty: a table
    // of its name alone), or taken out (`remove`), or - when there is none
    // and `add` - appended at the end. The rest of the file byte for byte.
    bool editFolderTable(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, StringView folder, StringView icon, bool remove, bool add);
}

bool setFolderIcon(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, StringView folder, StringView icon) {
    // The folder keeps its table with the icon gone: a saved folder stays
    // saved whatever its look.
    return editFolderTable(shelf, pool, identifier, folder, icon, false, true);
}

bool saveFolder(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, StringView folder) {
    if (shelf.style(folder) != nullptr) {
        return true;
    }
    return editFolderTable(shelf, pool, identifier, folder, StringView(), false, true);
}

bool forgetFolder(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, StringView folder) {
    return editFolderTable(shelf, pool, identifier, folder, StringView(), true, false);
}

namespace {
bool editFolderTable(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, StringView folder, StringView icon, bool remove, bool add) {
    if (shelf.path.empty() || folder.empty()) {
        return false;
    }
    Buffer text;
    readWhole(shelf.path, text);
    ObjPool::Ref scratch = ObjPool::fromMemory();
    Vector<Bookmark> entries;
    Vector<FolderStyle> styles;
    u64 nextId = 1;
    parseBookmarks(StringView(text), StringView(), StringView(), *scratch, nextId, entries, &styles);
    const FolderStyle* found = nullptr;
    for (const FolderStyle& style : styles) {
        if (style.name == folder) {
            found = &style;
            break;
        }
    }
    StringBuilder next;
    if (found != nullptr) {
        StringBuilder block;
        folderBlock(folder, icon, block);
        const StringView blockText(block);
        if (!spliceTable(StringView(text), StringView(u8"folder"), found->block, remove ? nullptr : &blockText, next)) {
            return false;
        }
    } else if (!add) {
        return true;
    } else {
        const StringView old(text);
        next << old;
        if (!old.empty()) {
            if (old[old.length() - 1] != '\n') {
                next << StringView(u8"\n");
            }
            next << StringView(u8"\n");
        }
        folderBlock(folder, icon, next);
    }
    if (!writeWhole(shelf.path, StringView(next))) {
        return false;
    }
    reloadBookmarks(shelf, pool, identifier);
    return true;
}
}

bool renameFolderInFile(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, StringView from, StringView to) {
    if (shelf.path.empty() || from.empty() || to.empty()) {
        return false;
    }
    Vector<u64> moving;
    for (const Bookmark& bookmark : shelf.items) {
        if (bookmark.folder == from) {
            moving.pushBack(bookmark.id);
        }
    }
    for (const u64 id : moving) {
        if (!setBookmarkFolder(shelf, pool, identifier, id, to)) {
            return false;
        }
    }
    const FolderStyle* const style = shelf.style(from);
    if (style == nullptr) {
        return true;
    }
    const StringView icon = style->icon;
    // Out under the old name, in under the new one - in place, since the
    // table is found by its name and the name is what changes.
    Buffer text;
    readWhole(shelf.path, text);
    ObjPool::Ref scratch = ObjPool::fromMemory();
    Vector<Bookmark> entries;
    Vector<FolderStyle> styles;
    u64 nextId = 1;
    parseBookmarks(StringView(text), StringView(), StringView(), *scratch, nextId, entries, &styles);
    for (const FolderStyle& entry : styles) {
        if (entry.name == from) {
            StringBuilder block;
            folderBlock(to, icon, block);
            const StringView blockText(block);
            StringBuilder next;
            if (!spliceTable(StringView(text), StringView(u8"folder"), entry.block, &blockText, next) || !writeWhole(shelf.path, StringView(next))) {
                return false;
            }
            break;
        }
    }
    reloadBookmarks(shelf, pool, identifier);
    return true;
}

bool deleteFolderInFile(BookmarkShelf& shelf, ObjPool& pool, StringView identifier, StringView folder, bool dropBookmarks) {
    if (shelf.path.empty() || folder.empty()) {
        return false;
    }
    Vector<u64> members;
    for (const Bookmark& bookmark : shelf.items) {
        if (bookmark.folder == folder) {
            members.pushBack(bookmark.id);
        }
    }
    for (const u64 id : members) {
        const bool done = dropBookmarks ? unpinBookmark(shelf, pool, identifier, id) : setBookmarkFolder(shelf, pool, identifier, id, StringView());
        if (!done) {
            return false;
        }
    }
    return forgetFolder(shelf, pool, identifier, folder);
}

bool setBookmarkTitle(BookmarkShelf& shelf, ObjPool& pool, u64 id, StringView title) {
    const size_t at = shelf.indexOf(id);
    if (at == shelf.items.length() || shelf.path.empty() || title.empty()) {
        return false;
    }
    Bookmark renamed = shelf.items[at];
    renamed.title = pool.intern(title);
    Buffer text;
    readWhole(shelf.path, text);
    StringBuilder next;
    if (!replaceBookmark(StringView(text), shelf.items[at], renamed, next) || !writeWhole(shelf.path, StringView(next))) {
        return false;
    }
    // Not reloaded: the title is part of what makes two bookmarks the same
    // one, so a reload would match nothing and hand out a new id.
    shelf.items.mut(at).title = renamed.title;
    return true;
}
