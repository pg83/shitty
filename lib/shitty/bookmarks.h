/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include "startup.h"

#include <std/lib/vector.h>
#include <std/str/view.h>
#include <std/sys/types.h>

namespace stl {
    class ObjPool;
    class StringBuilder;
}

// Bookmarks: tabs the user keeps - an ssh host, a project directory -
// listed at the top of the sidebar whether or not they are open, and
// opened by a click.
//
// They live in a file of their own beside the config, bookmarks.toml,
// holding nothing but [[bookmark]] tables:
//
//     [[bookmark]]
//     title = "prod"
//     command = "ssh prod"
//     dir = "~"
//
// A file of their own, and not keys in the config or a file the config
// imports, because the sidebar writes to it (pinning a tab appends a
// block): a file the program rewrites must not be one that also holds
// the user's other settings and their comments.
//
// Portable and free of AppKit, so everything but the drawing is in reach
// of a headless test.
struct Bookmark {
    // Stable for the life of the process and never 0, so a tab can name
    // the bookmark it was opened from and 0 can mean "none".
    u64 id = 0;
    stl::StringView title;
    // Run by the user's shell (`$SHELL -c command`); empty is the shell
    // itself, which is what a directory bookmark is.
    stl::StringView command;
    // Where the child starts; `~` and `~/` stand for home. Empty starts
    // wherever a new tab would.
    stl::StringView directory;
    // The sidebar folder it sits in (the `folder` key); empty for none.
    stl::StringView folder;
    // Which [[bookmark]] header of its file this entry came from, counted
    // from 0 over every header, the ones left out included: the block
    // unpinning cuts.
    u32 block = 0;
};

// A folder's look, saved in the same file as a [[folder]] table - its
// `name` and the SF Symbol `icon` the sidebar draws before it. A folder
// with no table has no icon; one with a table is kept even with nothing in
// it, because the user gave it a look.
struct FolderStyle {
    stl::StringView name;
    stl::StringView icon;
    // Which [[folder]] header of the file it came from, as Bookmark::block.
    u32 block = 0;
};

// bookmarks.toml in the directory of the config file this process
// resolved (Options::configPath), -config override included - the same
// derivation, for the same reason, as defaultQuickFramePath(). False, out
// untouched, when configPath is empty.
bool defaultBookmarksPath(stl::StringView configPath, stl::StringBuilder& out);

// Appends the [[bookmark]] entries of one document to `out`, their
// strings interned in `pool` and their ids counted up from `nextId`.
// Like the config, a problem never stops the terminal: an entry that is
// not understood is warned about on stderr, prefixed with `identifier`
// and `path` (nothing is said when `identifier` is empty), and left out,
// and a syntax error keeps the entries before it.
void parseBookmarks(stl::StringView text, stl::StringView identifier, stl::StringView path, stl::ObjPool& pool, u64& nextId, stl::Vector<Bookmark>& out, stl::Vector<FolderStyle>* folders = nullptr);

// parseBookmarks() over a file. A missing or unreadable file is no
// bookmarks and no warning: most users have none.
void loadBookmarks(stl::StringView path, stl::StringView identifier, stl::ObjPool& pool, u64& nextId, stl::Vector<Bookmark>& out, stl::Vector<FolderStyle>* folders = nullptr);

// What a bookmark's tab runs: `shell` - the user's shell as the process
// resolved it at startup, login or not as the `login` option said - with
// `-c command` after its arguments when there is a command. A login
// shell reads the profile first, so `ssh` finds the agent and the PATH it
// would from an ordinary tab. Pure: the shell was resolved (and SHELL
// set) once, before any thread existed, and is not resolved again here.
LaunchCommand bookmarkLaunchCommand(const LaunchCommand& shell, stl::StringView command);

// The window's bookmarks and where they came from - what the sidebar
// lists and what a tab opened from one names by id. One per process,
// on the composer, loaded at startup.
struct BookmarkShelf {
    stl::Vector<Bookmark> items;
    // The file's [[folder]] tables, in its order.
    stl::Vector<FolderStyle> folders;
    // The file the items were read from and pins go to; empty when no
    // path could be computed (no config path at all).
    stl::StringView path;
    u64 nextId = 1;

    // The bookmark with this id, or null.
    const Bookmark* find(u64 id) const;
    // Its position in items, or items.length() when there is none.
    size_t indexOf(u64 id) const;
    // The style saved for a folder, or null.
    const FolderStyle* style(stl::StringView folder) const;
};

// Where a bookmark stands, as the sidebar says it: no tab; a tab whose
// child runs; a tab whose child has exited (kept, a click reconnects);
// and, for one that is not open, a host that does not answer.
enum class BookmarkState : u8 {
    Closed,
    Open,
    Exited,
    Unreachable,
};

// The line under a bookmark's title in the sidebar: the state, then what
// it runs, or where when it runs only the shell - "open · ssh prod",
// "not open · ~/Projects/shitty", "exited · click to reconnect".
// Replaces what `out` held.
void bookmarkStatus(const Bookmark& bookmark, BookmarkState state, stl::StringBuilder& out);

// Pinning and unpinning: the file is changed a whole block at a time and
// everything else in it - comments, blank lines, entries this process
// did not understand - is kept byte for byte.
//
// The block for one bookmark, TOML-escaped, ending in a newline; a key
// with nothing in it is left out.
void bookmarkBlock(const Bookmark& bookmark, stl::StringBuilder& out);
// `text` with the bookmark's block added at the end, a blank line before
// it. Replaces what `out` held.
void appendBookmark(stl::StringView text, const Bookmark& bookmark, stl::StringBuilder& out);
// `text` without the block of the first entry equal to `bookmark` (by
// title, command and dir): from its [[bookmark]] line to the next table
// header or the end. Comments inside that span go with it. False, out
// untouched, when no such entry is in the text.
bool removeBookmark(stl::StringView text, const Bookmark& bookmark, stl::StringBuilder& out);
// `text` with that entry's block replaced by the block of `replacement`,
// in the same place - so the shelf's order, which is the file's, does not
// move. False, out untouched, when no such entry is in the text.
bool replaceBookmark(stl::StringView text, const Bookmark& bookmark, const Bookmark& replacement, stl::StringBuilder& out);
// Same title, command and dir; the folder is where a bookmark is, not
// which one it is.
bool sameBookmark(const Bookmark& a, const Bookmark& b);

// A command line as a shell would need it typed: the NUL-separated
// arguments processCommandLine() reads, joined by spaces, each one that
// the shell would split or expand in single quotes. Replaces what `out`
// held.
void shellCommandLine(stl::StringView arguments, stl::StringBuilder& out);

// The shelf against its file again: entries equal to ones it holds keep
// their ids, so a tab opened from one keeps naming it; the rest are new.
void reloadBookmarks(BookmarkShelf& shelf, stl::ObjPool& pool, stl::StringView identifier);
// Adds a bookmark to the shelf's file and the shelf; its new id in `id`.
// False when there is no file to write or it could not be written, and
// then nothing changed.
bool pinBookmark(BookmarkShelf& shelf, stl::ObjPool& pool, stl::StringView identifier, const Bookmark& bookmark, u64& id);
// Takes the bookmark with this id out of the file and the shelf. False
// when it is not on the shelf, or the file no longer holds it, or the
// file could not be written; then nothing changed.
bool unpinBookmark(BookmarkShelf& shelf, stl::ObjPool& pool, stl::StringView identifier, u64 id);
// Moves the bookmark with this id into `folder` (empty: out of any), in
// its file and on the shelf, its block rewritten in place. False when it is
// not on the shelf or the file could not be written; then nothing changed.
bool setBookmarkFolder(BookmarkShelf& shelf, stl::ObjPool& pool, stl::StringView identifier, u64 id, stl::StringView folder);

// Folders: the sidebar's order of them. Every folder a bookmark on the
// shelf names, in the order the shelf first names them - they are saved,
// and the file says where they go - then the window's own, the ones made
// in the sidebar, in the order they were made. No name twice, none empty.
// Replaces what `out` held.
void folderOrder(const BookmarkShelf* shelf, const stl::Vector<stl::StringView>& windowFolders, stl::Vector<stl::StringView>& out);
// A folder's place in that order, or order.length() for none of them.
size_t folderIndex(const stl::Vector<stl::StringView>& order, stl::StringView folder);

// Gives a folder its icon in the file: its [[folder]] table rewritten in
// place, or one added at the end; an empty icon leaves the table with the
// name alone, so the folder stays saved. The shelf is reloaded. False when there is no file or it could not be
// written; then nothing changed.
bool setFolderIcon(BookmarkShelf& shelf, stl::ObjPool& pool, stl::StringView identifier, stl::StringView folder, stl::StringView icon);
// Saves a folder in the file - a [[folder]] table of its name - so it is
// in the list after a restart, empty or holding only tabs. Nothing changes
// for a folder already saved. The shelf is reloaded.
bool saveFolder(BookmarkShelf& shelf, stl::ObjPool& pool, stl::StringView identifier, stl::StringView folder);
// Takes a folder's [[folder]] table out of the file, when it has one.
bool forgetFolder(BookmarkShelf& shelf, stl::ObjPool& pool, stl::StringView identifier, stl::StringView folder);
// Renames a folder in the file: its [[folder]] table and every bookmark
// naming it, each rewritten in place. False when there is no file or it
// could not be written; then nothing changed.
bool renameFolderInFile(BookmarkShelf& shelf, stl::ObjPool& pool, stl::StringView identifier, stl::StringView from, stl::StringView to);
// Deletes a folder from the file: its [[folder]] table goes, and every
// bookmark naming it either leaves it (its block rewritten in place, its id
// kept) or, with dropBookmarks, goes too. The shelf is reloaded. False when
// there is no file or it could not be written; entries edited before the
// failure stay edited.
bool deleteFolderInFile(BookmarkShelf& shelf, stl::ObjPool& pool, stl::StringView identifier, stl::StringView folder, bool dropBookmarks);
// Renames the bookmark with this id, in its file (its block rewritten in
// place) and on the shelf, keeping its id - a tab opened from it goes on
// naming it. False when it is not on the shelf or the file could not be
// written; then nothing changed.
bool setBookmarkTitle(BookmarkShelf& shelf, stl::ObjPool& pool, u64 id, stl::StringView title);
