/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "bookmarks.h"

#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/tst/ut.h>
#include <std/ios/fs_utils.h>
#include <std/lib/buffer.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

using namespace stl;

namespace {
    // A mkdtemp() directory of this test's own, as quick_frame_store_ut
    // makes one: TMPDIR when the runner gives one, which is inside the
    // tree under ./build.
    void makeTempDir(StringBuilder& dir) {
        const char* const directory = getenv("TMPDIR");
        dir << StringView(directory != nullptr ? directory : "/tmp") << StringView(u8"/bookmarks_ut.XXXXXX");
        STD_INSIST(mkdtemp(dir.cStr()) != nullptr);
    }

    void readAll(StringView path, Buffer& out) {
        out.reset();
        Buffer pathBuf{path};
        readFileContent(pathBuf, out);
    }

    void parse(StringView text, Vector<Bookmark>& out, u64& nextId) {
        static ObjPool::Ref pool = ObjPool::fromMemory();
        parseBookmarks(text, StringView(u8"shitty"), StringView(u8"bookmarks.toml"), *pool, nextId, out);
    }
}

STD_TEST_SUITE(Bookmarks) {
    STD_TEST(EveryFieldOfEveryEntryIsRead) {
        Vector<Bookmark> out;
        u64 nextId = 5;
        parse(StringView(u8"# mine\n"
                         "[[bookmark]]\n"
                         "title = \"prod\"\n"
                         "command = \"ssh prod\"\n"
                         "dir = \"~\"\n"
                         "\n"
                         "[[bookmark]]\n"
                         "title = \"shitty\"\n"
                         "dir = \"~/Projects/shitty\"\n"),
              out, nextId);
        STD_INSIST(out.length() == 2);
        STD_INSIST(out[0].title == StringView(u8"prod"));
        STD_INSIST(out[0].command == StringView(u8"ssh prod"));
        STD_INSIST(out[0].directory == StringView(u8"~"));
        STD_INSIST(out[1].title == StringView(u8"shitty"));
        STD_INSIST(out[1].command.empty());
        STD_INSIST(out[1].directory == StringView(u8"~/Projects/shitty"));
        // Ids count up from where the caller is, distinct and never 0.
        STD_INSIST(out[0].id == 5 && out[1].id == 6 && nextId == 7);
    }

    // A title is optional: the command names the bookmark, else the dir.
    STD_TEST(AnUntitledBookmarkIsNamedByWhatItOpens) {
        Vector<Bookmark> out;
        u64 nextId = 1;
        parse(StringView(u8"[[bookmark]]\ncommand = \"ssh a\"\n[[bookmark]]\ndir = \"/srv\"\n"), out, nextId);
        STD_INSIST(out.length() == 2);
        STD_INSIST(out[0].title == StringView(u8"ssh a"));
        STD_INSIST(out[1].title == StringView(u8"/srv"));
    }

    // What is not understood is left out, and only that: every broken
    // entry sits between two good ones, which must both survive.
    STD_TEST(AnEntryNotUnderstoodIsLeftOutAndOnlyIt) {
        Vector<Bookmark> out;
        u64 nextId = 1;
        parse(StringView(u8"[[bookmark]]\ntitle = \"first\"\ncommand = \"a\"\n"
                         "[[bookmark]]\ntitle = \"nothing to open\"\n"
                         "[[bookmark]]\ntitle = \"number\"\ncommand = 3\n"
                         "[[bookmark]]\ntitle = \"list\"\ncommand = [\"a\", \"b\"]\n"
                         "[bookmark]\ntitle = \"not an array\"\ncommand = \"x\"\n"
                         "[other]\ncommand = \"y\"\n"
                         "[[bookmark]]\ntitle = \"last\"\ndir = \"/\"\nextra = \"ignored\"\n"),
              out, nextId);
        STD_INSIST(out.length() == 2);
        STD_INSIST(out[0].title == StringView(u8"first"));
        STD_INSIST(out[1].title == StringView(u8"last"));
        STD_INSIST(out[1].directory == StringView(u8"/"));
    }

    STD_TEST(KeysOutsideATableAreNotABookmark) {
        Vector<Bookmark> out;
        u64 nextId = 1;
        parse(StringView(u8"command = \"ssh a\"\n"), out, nextId);
        STD_INSIST(out.length() == 0);
        STD_INSIST(nextId == 1);
    }

    // A syntax error keeps what came before it.
    STD_TEST(ASyntaxErrorKeepsTheEntriesBeforeIt) {
        Vector<Bookmark> out;
        u64 nextId = 1;
        parse(StringView(u8"[[bookmark]]\ncommand = \"a\"\n[[bookmark]]\ncommand = \"b\n"), out, nextId);
        STD_INSIST(out.length() == 1);
        STD_INSIST(out[0].command == StringView(u8"a"));
    }

    STD_TEST(AMissingFileIsNoBookmarks) {
        static ObjPool::Ref pool = ObjPool::fromMemory();
        Vector<Bookmark> out;
        u64 nextId = 1;
        loadBookmarks(StringView(u8"/nonexistent/bookmarks.toml"), StringView(u8"shitty"), *pool, nextId, out);
        STD_INSIST(out.length() == 0);
        loadBookmarks(StringView(), StringView(u8"shitty"), *pool, nextId, out);
        STD_INSIST(out.length() == 0);
    }

    // Beside the config, whatever the config file is called: the brand's
    // own name for it must not leak into the bookmarks file's.
    STD_TEST(TheFileSitsBesideTheConfig) {
        StringBuilder shitty;
        STD_INSIST(defaultBookmarksPath(StringView(u8"/home/u/.config/shitty/shitty.toml"), shitty));
        STD_INSIST(StringView(shitty) == StringView(u8"/home/u/.config/shitty/bookmarks.toml"));
        StringBuilder pretty;
        STD_INSIST(defaultBookmarksPath(StringView(u8"/home/u/.config/pretty/pretty.toml"), pretty));
        STD_INSIST(StringView(pretty) == StringView(u8"/home/u/.config/pretty/bookmarks.toml"));
        StringBuilder bare;
        STD_INSIST(defaultBookmarksPath(StringView(u8"cfg.toml"), bare));
        STD_INSIST(StringView(bare) == StringView(u8"bookmarks.toml"));
        StringBuilder none;
        STD_INSIST(!defaultBookmarksPath(StringView(), none));
        STD_INSIST(StringView(none).empty());
    }

    // The shell's own arguments stay - a login shell keeps its dash - and
    // -c goes after them.
    STD_TEST(TheCommandRunsThroughTheShellAsItWasResolved) {
        LaunchCommand shell;
        shell.storage.append("/bin/zsh", 9);
        shell.executableOffset = 0;
        shell.offsets.pushBack((u32)(shell.storage.used()));
        shell.storage.append("-zsh", 5);

        const LaunchCommand command = bookmarkLaunchCommand(shell, StringView(u8"ssh prod"));
        STD_INSIST(StringView(command.executable()) == StringView(u8"/bin/zsh"));
        STD_INSIST(command.offsets.length() == 3);
        STD_INSIST(StringView(command.argument(0)) == StringView(u8"-zsh"));
        STD_INSIST(StringView(command.argument(1)) == StringView(u8"-c"));
        STD_INSIST(StringView(command.argument(2)) == StringView(u8"ssh prod"));

        const LaunchCommand bare = bookmarkLaunchCommand(shell, StringView());
        STD_INSIST(bare.offsets.length() == 1);
        STD_INSIST(StringView(bare.argument(0)) == StringView(u8"-zsh"));
    }

    STD_TEST(TheStatusSaysOpenAndWhatItRuns) {
        const Bookmark ssh{1, StringView(u8"prod"), StringView(u8"ssh prod"), StringView(u8"~")};
        const Bookmark folder{2, StringView(u8"shitty"), StringView(), StringView(u8"~/Projects/shitty")};
        StringBuilder out;
        bookmarkStatus(ssh, BookmarkState::Open, out);
        STD_INSIST(StringView(out) == StringView(u8"open · ssh prod"));
        bookmarkStatus(ssh, BookmarkState::Closed, out);
        STD_INSIST(StringView(out) == StringView(u8"not open · ssh prod"));
        bookmarkStatus(folder, BookmarkState::Closed, out);
        STD_INSIST(StringView(out) == StringView(u8"not open · ~/Projects/shitty"));
        bookmarkStatus(ssh, BookmarkState::Exited, out);
        STD_INSIST(StringView(out) == StringView(u8"exited · click to reconnect"));
        bookmarkStatus(ssh, BookmarkState::Unreachable, out);
        STD_INSIST(StringView(out) == StringView(u8"unreachable · ssh prod"));
    }

    STD_TEST(TheShelfFindsByIdAndNothingElse) {
        BookmarkShelf shelf;
        shelf.items.pushBack(Bookmark{4, StringView(u8"a"), StringView(u8"x"), StringView()});
        shelf.items.pushBack(Bookmark{9, StringView(u8"b"), StringView(u8"y"), StringView()});
        STD_INSIST(shelf.indexOf(9) == 1);
        STD_INSIST(shelf.find(4) == &shelf.items[0]);
        STD_INSIST(shelf.find(0) == nullptr);
        STD_INSIST(shelf.indexOf(5) == 2);
    }
    // What pinning writes, the parser reads back as it was: quotes,
    // backslashes and a control character included.
    STD_TEST(AWrittenBlockReadsBackAsItWas) {
        const Bookmark odd{0, StringView(u8"say \"hi\" \\ there"), StringView(u8"printf 'a\tb'"), StringView(u8"/tmp/x y")};
        StringBuilder text;
        appendBookmark(StringView(), odd, text);
        Vector<Bookmark> out;
        u64 nextId = 1;
        parse(StringView(text), out, nextId);
        STD_INSIST(out.length() == 1);
        STD_INSIST(sameBookmark(out[0], odd));
        // An empty key is left out rather than written empty.
        const Bookmark folder{0, StringView(u8"f"), StringView(), StringView(u8"/srv")};
        StringBuilder block;
        bookmarkBlock(folder, block);
        STD_INSIST(StringView(block) == StringView(u8"[[bookmark]]\ntitle = \"f\"\ndir = \"/srv\"\n"));
    }

    // Appending keeps every byte already there, adds the missing newline
    // and one blank line, and nothing else.
    STD_TEST(AppendingKeepsTheFileAndAddsOneBlock) {
        const Bookmark b{0, StringView(u8"b"), StringView(u8"ssh b"), StringView()};
        StringBuilder out;
        appendBookmark(StringView(u8"# mine\n[[bookmark]]\ncommand = \"a\""), b, out);
        STD_INSIST(StringView(out) == StringView(u8"# mine\n[[bookmark]]\ncommand = \"a\"\n\n[[bookmark]]\ntitle = \"b\"\ncommand = \"ssh b\"\n"));
    }

    // Removing cuts the one block - the right one of two equal-looking
    // neighbours, found through the parser and not by text - and keeps
    // the comment that introduces the next block.
    STD_TEST(RemovingCutsOneBlockAndKeepsEverythingElse) {
        const StringView text(u8"# top\n"
                              "[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\n"
                              "\n"
                              "[[bookmark]]\ntitle = \"broken\"\ncommand = 3\n"
                              "\n"
                              "[[bookmark]]\ntitle = \"b\"\ncommand = \"x\"\n"
                              "# about c\n"
                              "[[bookmark]]\ntitle = \"c\"\ncommand = \"y\"\n");
        const Bookmark b{0, StringView(u8"b"), StringView(u8"x"), StringView()};
        StringBuilder out;
        STD_INSIST(removeBookmark(text, b, out));
        STD_INSIST(StringView(out) == StringView(u8"# top\n"
                                                 "[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\n"
                                                 "\n"
                                                 "[[bookmark]]\ntitle = \"broken\"\ncommand = 3\n"
                                                 "\n"
                                                 "# about c\n"
                                                 "[[bookmark]]\ntitle = \"c\"\ncommand = \"y\"\n"));
        const Bookmark missing{0, StringView(u8"zz"), StringView(u8"x"), StringView()};
        StringBuilder untouched;
        untouched << StringView(u8"keep");
        STD_INSIST(!removeBookmark(text, missing, untouched));
        STD_INSIST(StringView(untouched) == StringView(u8"keep"));
    }

    STD_TEST(ACommandLineIsQuotedOnlyWhereTheShellNeedsIt) {
        static const char ssh[] = "ssh\0-p\0" "2222\0user@prod.example";
        static const char vim[] = "vim\0my file\0it's\0";
        StringBuilder out;
        // sizeof counts the literal's own terminator: the last argument's.
        shellCommandLine(StringView((const u8*)(ssh), sizeof(ssh)), out);
        STD_INSIST(StringView(out) == StringView(u8"ssh -p 2222 user@prod.example"));
        shellCommandLine(StringView((const u8*)(vim), sizeof(vim)), out);
        STD_INSIST(StringView(out) == StringView(u8"vim 'my file' 'it'\\''s' ''"));
    }

    // Pin writes the file and the shelf, unpin takes it out of both, and
    // an entry that was there before keeps its id through both - a tab
    // opened from it has to go on naming it.
    STD_TEST(PinAndUnpinGoThroughTheFileAndKeepTheOtherIds) {
        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder path;
        path << StringView(dir) << StringView(u8"/sub/bookmarks.toml");
        static ObjPool::Ref pool = ObjPool::fromMemory();
        BookmarkShelf shelf;
        shelf.path = pool->intern(StringView(path));

        // No file and no directory yet: pinning makes both.
        const Bookmark a{0, StringView(u8"a"), StringView(u8"ssh a"), StringView()};
        u64 first = 0;
        STD_INSIST(pinBookmark(shelf, *pool, StringView(), a, first));
        STD_INSIST(first != 0 && shelf.items.length() == 1 && shelf.items[0].id == first);

        const Bookmark b{0, StringView(u8"b"), StringView(), StringView(u8"/srv")};
        u64 second = 0;
        STD_INSIST(pinBookmark(shelf, *pool, StringView(), b, second));
        STD_INSIST(second != 0 && second != first);
        STD_INSIST(shelf.items.length() == 2);
        STD_INSIST(shelf.find(first) != nullptr && sameBookmark(*shelf.find(first), a));

        STD_INSIST(unpinBookmark(shelf, *pool, StringView(), first));
        STD_INSIST(shelf.items.length() == 1);
        STD_INSIST(shelf.items[0].id == second);
        Buffer written;
        readAll(StringView(path), written);
        STD_INSIST(StringView(written) == StringView(u8"[[bookmark]]\ntitle = \"b\"\ndir = \"/srv\"\n"));
        STD_INSIST(!unpinBookmark(shelf, *pool, StringView(), first));

        StringBuilder sub;
        sub << StringView(dir) << StringView(u8"/sub");
        Buffer file{StringView(path)};
        Buffer subBuf{StringView(sub)};
        Buffer dirBuf{StringView(dir)};
        STD_INSIST(unlink(file.cStr()) == 0);
        STD_INSIST(rmdir(subBuf.cStr()) == 0);
        STD_INSIST(rmdir(dirBuf.cStr()) == 0);
    }

    STD_TEST(NothingIsPinnedWithoutAFile) {
        static ObjPool::Ref pool = ObjPool::fromMemory();
        BookmarkShelf shelf;
        u64 id = 0;
        STD_INSIST(!pinBookmark(shelf, *pool, StringView(), Bookmark{0, StringView(u8"a"), StringView(u8"a"), StringView()}, id));
        STD_INSIST(shelf.items.length() == 0 && id == 0);
    }
    // The folder key is read and written like the others, and a block
    // replaced in place keeps its neighbours and its own place.
    STD_TEST(AFolderIsReadWrittenAndReplacedInPlace) {
        Vector<Bookmark> out;
        u64 nextId = 1;
        parse(StringView(u8"[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\nfolder = \"servers\"\n"), out, nextId);
        STD_INSIST(out.length() == 1 && out[0].folder == StringView(u8"servers"));
        StringBuilder block;
        bookmarkBlock(out[0], block);
        STD_INSIST(StringView(block) == StringView(u8"[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\nfolder = \"servers\"\n"));

        const StringView text(u8"# top\n"
                              "[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\n"
                              "\n"
                              "[[bookmark]]\ntitle = \"b\"\ncommand = \"y\"\n"
                              "# last\n");
        const Bookmark a{0, StringView(u8"a"), StringView(u8"x"), StringView()};
        Bookmark moved = a;
        moved.folder = StringView(u8"work");
        StringBuilder next;
        STD_INSIST(replaceBookmark(text, a, moved, next));
        STD_INSIST(StringView(next) == StringView(u8"# top\n"
                                                  "[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\nfolder = \"work\"\n"
                                                  "\n"
                                                  "[[bookmark]]\ntitle = \"b\"\ncommand = \"y\"\n"
                                                  "# last\n"));
        // The folder is where a bookmark is, not which one it is.
        STD_INSIST(sameBookmark(a, moved));
    }

    // Saved folders first, in the order the shelf first names them, then
    // the window's own; no name twice and no empty one.
    STD_TEST(FoldersAreOrderedByTheShelfThenByTheWindow) {
        BookmarkShelf shelf;
        shelf.items.pushBack(Bookmark{1, StringView(u8"a"), StringView(u8"x"), StringView(), StringView(u8"servers")});
        shelf.items.pushBack(Bookmark{2, StringView(u8"b"), StringView(u8"x"), StringView(), StringView()});
        shelf.items.pushBack(Bookmark{3, StringView(u8"c"), StringView(u8"x"), StringView(), StringView(u8"db")});
        shelf.items.pushBack(Bookmark{4, StringView(u8"d"), StringView(u8"x"), StringView(), StringView(u8"servers")});
        Vector<StringView> window;
        window.pushBack(StringView(u8"work"));
        window.pushBack(StringView(u8"db"));
        window.pushBack(StringView());
        Vector<StringView> order;
        folderOrder(&shelf, window, order);
        STD_INSIST(order.length() == 3);
        STD_INSIST(order[0] == StringView(u8"servers"));
        STD_INSIST(order[1] == StringView(u8"db"));
        STD_INSIST(order[2] == StringView(u8"work"));
        STD_INSIST(folderIndex(order, StringView(u8"db")) == 1);
        STD_INSIST(folderIndex(order, StringView(u8"nope")) == 3);
        folderOrder(nullptr, window, order);
        STD_INSIST(order.length() == 2 && order[0] == StringView(u8"work"));
    }

    // Moving a bookmark between folders goes through its file, keeps its id
    // and its place on the shelf.
    STD_TEST(ABookmarkMovesBetweenFoldersInItsFile) {
        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder path;
        path << StringView(dir) << StringView(u8"/bookmarks.toml");
        static ObjPool::Ref pool = ObjPool::fromMemory();
        BookmarkShelf shelf;
        shelf.path = pool->intern(StringView(path));
        u64 a = 0;
        u64 b = 0;
        STD_INSIST(pinBookmark(shelf, *pool, StringView(), Bookmark{0, StringView(u8"a"), StringView(u8"x"), StringView()}, a));
        STD_INSIST(pinBookmark(shelf, *pool, StringView(), Bookmark{0, StringView(u8"b"), StringView(u8"y"), StringView()}, b));
        STD_INSIST(setBookmarkFolder(shelf, *pool, StringView(), a, StringView(u8"servers")));
        STD_INSIST(shelf.items.length() == 2);
        STD_INSIST(shelf.items[0].id == a && shelf.items[0].folder == StringView(u8"servers"));
        STD_INSIST(shelf.items[1].id == b && shelf.items[1].folder.empty());
        STD_INSIST(setBookmarkFolder(shelf, *pool, StringView(), a, StringView()));
        STD_INSIST(shelf.items[0].id == a && shelf.items[0].folder.empty());
        STD_INSIST(!setBookmarkFolder(shelf, *pool, StringView(), 999, StringView(u8"x")));
        Buffer file{StringView(path)};
        Buffer dirBuf{StringView(dir)};
        STD_INSIST(unlink(file.cStr()) == 0);
        STD_INSIST(rmdir(dirBuf.cStr()) == 0);
    }
    // A folder's look is a [[folder]] table beside the bookmarks: read,
    // added, replaced in place, taken out; and a folder kept only for its
    // look still has its place in the order.
    STD_TEST(AFolderIconLivesInAFolderTable) {
        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder path;
        path << StringView(dir) << StringView(u8"/bookmarks.toml");
        static ObjPool::Ref pool = ObjPool::fromMemory();
        BookmarkShelf shelf;
        shelf.path = pool->intern(StringView(path));
        u64 a = 0;
        STD_INSIST(pinBookmark(shelf, *pool, StringView(), Bookmark{0, StringView(u8"a"), StringView(u8"x"), StringView(), StringView(u8"servers")}, a));

        STD_INSIST(setFolderIcon(shelf, *pool, StringView(), StringView(u8"servers"), StringView(u8"server.rack")));
        STD_INSIST(setFolderIcon(shelf, *pool, StringView(), StringView(u8"empty"), StringView(u8"star")));
        STD_INSIST(shelf.style(StringView(u8"servers")) != nullptr);
        STD_INSIST(shelf.style(StringView(u8"servers"))->icon == StringView(u8"server.rack"));
        Vector<StringView> order;
        const Vector<StringView> none;
        folderOrder(&shelf, none, order);
        STD_INSIST(order.length() == 2 && order[0] == StringView(u8"servers") && order[1] == StringView(u8"empty"));

        // Replaced in place: the file keeps its order.
        STD_INSIST(setFolderIcon(shelf, *pool, StringView(), StringView(u8"servers"), StringView(u8"globe")));
        Buffer written;
        readAll(StringView(path), written);
        STD_INSIST(StringView(written) == StringView(u8"[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\nfolder = \"servers\"\n"
                                                     "\n[[folder]]\nname = \"servers\"\nicon = \"globe\"\n"
                                                     "\n[[folder]]\nname = \"empty\"\nicon = \"star\"\n"));

        // Renamed: the table and the bookmark both move to the new name, and
        // the bookmark keeps its id.
        STD_INSIST(renameFolderInFile(shelf, *pool, StringView(), StringView(u8"servers"), StringView(u8"hosts")));
        STD_INSIST(shelf.items[0].id == a && shelf.items[0].folder == StringView(u8"hosts"));
        STD_INSIST(shelf.style(StringView(u8"hosts")) != nullptr && shelf.style(StringView(u8"hosts"))->icon == StringView(u8"globe"));
        STD_INSIST(shelf.style(StringView(u8"servers")) == nullptr);

        // No icon: the table keeps the folder's name, so the folder stays
        // saved; forgetting it takes the table out.
        STD_INSIST(setFolderIcon(shelf, *pool, StringView(), StringView(u8"empty"), StringView()));
        STD_INSIST(shelf.style(StringView(u8"empty")) != nullptr && shelf.style(StringView(u8"empty"))->icon.empty());
        readAll(StringView(path), written);
        STD_INSIST(StringView(written) == StringView(u8"[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\nfolder = \"hosts\"\n"
                                                     "\n[[folder]]\nname = \"hosts\"\nicon = \"globe\"\n"
                                                     "\n[[folder]]\nname = \"empty\"\n"));
        STD_INSIST(forgetFolder(shelf, *pool, StringView(), StringView(u8"empty")));
        STD_INSIST(shelf.style(StringView(u8"empty")) == nullptr);
        readAll(StringView(path), written);
        STD_INSIST(StringView(written) == StringView(u8"[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\nfolder = \"hosts\"\n"
                                                     "\n[[folder]]\nname = \"hosts\"\nicon = \"globe\"\n"));

        Buffer file{StringView(path)};
        Buffer dirBuf{StringView(dir)};
        STD_INSIST(unlink(file.cStr()) == 0);
        STD_INSIST(rmdir(dirBuf.cStr()) == 0);
    }
    // A bookmark renamed keeps its id and its place, in the file and on the
    // shelf; a later reload still knows it.
    STD_TEST(ARenamedBookmarkKeepsItsId) {
        StringBuilder dir;
        makeTempDir(dir);
        StringBuilder path;
        path << StringView(dir) << StringView(u8"/bookmarks.toml");
        static ObjPool::Ref pool = ObjPool::fromMemory();
        BookmarkShelf shelf;
        shelf.path = pool->intern(StringView(path));
        u64 a = 0;
        u64 b = 0;
        STD_INSIST(pinBookmark(shelf, *pool, StringView(), Bookmark{0, StringView(u8"ssh prod"), StringView(u8"ssh prod"), StringView()}, a));
        STD_INSIST(pinBookmark(shelf, *pool, StringView(), Bookmark{0, StringView(u8"b"), StringView(u8"y"), StringView()}, b));
        STD_INSIST(setBookmarkTitle(shelf, *pool, a, StringView(u8"prod")));
        STD_INSIST(shelf.items[0].id == a && shelf.items[0].title == StringView(u8"prod"));
        Buffer written;
        readAll(StringView(path), written);
        STD_INSIST(StringView(written) == StringView(u8"[[bookmark]]\ntitle = \"prod\"\ncommand = \"ssh prod\"\n"
                                                     "\n[[bookmark]]\ntitle = \"b\"\ncommand = \"y\"\n"));
        reloadBookmarks(shelf, *pool, StringView());
        STD_INSIST(shelf.items[0].id == a && shelf.items[1].id == b);
        STD_INSIST(!setBookmarkTitle(shelf, *pool, 999, StringView(u8"x")));
        STD_INSIST(!setBookmarkTitle(shelf, *pool, a, StringView()));
        Buffer file{StringView(path)};
        Buffer dirBuf{StringView(dir)};
        STD_INSIST(unlink(file.cStr()) == 0);
        STD_INSIST(rmdir(dirBuf.cStr()) == 0);
    }
    // Deleting a folder: its table goes, and its bookmarks either leave it
    // (same ids, blocks rewritten in place) or go with it. The other folder,
    // its bookmark and the comment are kept byte for byte.
    STD_TEST(ADeletedFolderLetsGoOfOrTakesItsBookmarks) {
        const StringView original(u8"# mine\n"
                                  "[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\nfolder = \"servers\"\n"
                                  "\n[[bookmark]]\ntitle = \"b\"\ncommand = \"y\"\nfolder = \"other\"\n"
                                  "\n[[bookmark]]\ntitle = \"c\"\ncommand = \"z\"\nfolder = \"servers\"\n"
                                  "\n[[folder]]\nname = \"servers\"\nicon = \"globe\"\n");
        for (int round = 0; round < 2; ++round) {
            const bool drop = round == 1;
            StringBuilder dir;
            makeTempDir(dir);
            StringBuilder path;
            path << StringView(dir) << StringView(u8"/bookmarks.toml");
            Buffer file{StringView(path)};
            FILE* const out = fopen(file.cStr(), "w");
            STD_INSIST(out != nullptr);
            STD_INSIST(fwrite(original.data(), 1, original.length(), out) == original.length());
            STD_INSIST(fclose(out) == 0);
            static ObjPool::Ref pool = ObjPool::fromMemory();
            BookmarkShelf shelf;
            shelf.path = pool->intern(StringView(path));
            reloadBookmarks(shelf, *pool, StringView());
            // Premise: two folders, the deleted one holding two bookmarks
            // and a table, the other one a bookmark of its own.
            STD_INSIST(shelf.items.length() == 3);
            STD_INSIST(shelf.items[0].folder == StringView(u8"servers") && shelf.items[1].folder == StringView(u8"other") && shelf.items[2].folder == StringView(u8"servers"));
            STD_INSIST(shelf.style(StringView(u8"servers")) != nullptr);
            const u64 a = shelf.items[0].id;
            const u64 b = shelf.items[1].id;
            const u64 c = shelf.items[2].id;

            STD_INSIST(deleteFolderInFile(shelf, *pool, StringView(), StringView(u8"servers"), drop));
            STD_INSIST(shelf.style(StringView(u8"servers")) == nullptr);
            Buffer written;
            readAll(StringView(path), written);
            if (drop) {
                STD_INSIST(shelf.items.length() == 1);
                STD_INSIST(shelf.items[0].id == b && shelf.items[0].folder == StringView(u8"other"));
                STD_INSIST(StringView(written) == StringView(u8"# mine\n"
                                                             "[[bookmark]]\ntitle = \"b\"\ncommand = \"y\"\nfolder = \"other\"\n"));
            } else {
                STD_INSIST(shelf.items.length() == 3);
                STD_INSIST(shelf.items[0].id == a && shelf.items[0].folder.empty());
                STD_INSIST(shelf.items[1].id == b && shelf.items[1].folder == StringView(u8"other"));
                STD_INSIST(shelf.items[2].id == c && shelf.items[2].folder.empty());
                STD_INSIST(StringView(written) == StringView(u8"# mine\n"
                                                             "[[bookmark]]\ntitle = \"a\"\ncommand = \"x\"\n"
                                                             "\n[[bookmark]]\ntitle = \"b\"\ncommand = \"y\"\nfolder = \"other\"\n"
                                                             "\n[[bookmark]]\ntitle = \"c\"\ncommand = \"z\"\n"));
            }
            Vector<StringView> order;
            const Vector<StringView> none;
            folderOrder(&shelf, none, order);
            STD_INSIST(order.length() == 1 && order[0] == StringView(u8"other"));
            Buffer dirBuf{StringView(dir)};
            STD_INSIST(unlink(file.cStr()) == 0);
            STD_INSIST(rmdir(dirBuf.cStr()) == 0);
        }
    }
}
