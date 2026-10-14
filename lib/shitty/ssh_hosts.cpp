/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "ssh_hosts.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>

#include <fcntl.h>
#include <glob.h>
#include <unistd.h>

using namespace stl;

namespace {
    bool space(u8 c) {
        return c == ' ' || c == '\t' || c == '\r';
    }

    u8 lower(u8 c) {
        return c >= 'A' && c <= 'Z' ? (u8)(c - 'A' + 'a') : c;
    }

    bool keyIs(StringView key, const char* name) {
        size_t at = 0;
        for (; name[at] != '\0'; ++at) {
            if (at >= key.length() || lower((u8)(key.data()[at])) != (u8)(name[at])) {
                return false;
            }
        }
        return at == key.length();
    }

    // The words after the keyword: split at blanks, a double-quoted word
    // kept whole.
    void words(StringView rest, Vector<StringView>& out) {
        out.clear();
        const u8* p = (const u8*)(rest.data());
        const u8* const end = p + rest.length();
        while (p < end) {
            while (p < end && space(*p)) {
                ++p;
            }
            if (p >= end) {
                break;
            }
            if (*p == '"') {
                const u8* const start = ++p;
                while (p < end && *p != '"') {
                    ++p;
                }
                out.pushBack(StringView(start, (size_t)(p - start)));
                if (p < end) {
                    ++p;
                }
                continue;
            }
            const u8* const start = p;
            while (p < end && !space(*p)) {
                ++p;
            }
            out.pushBack(StringView(start, (size_t)(p - start)));
        }
    }

    bool pattern(StringView name) {
        for (size_t at = 0; at < name.length(); ++at) {
            const char c = name.data()[at];
            if (c == '*' || c == '?' || c == '!') {
                return true;
            }
        }
        return false;
    }

    void readFile(const char* path, Buffer& out) {
        out.reset();
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        char chunk[4096];
        for (;;) {
            const ssize_t got = read(fd, chunk, sizeof(chunk));
            if (got <= 0) {
                break;
            }
            out.append(chunk, (size_t)(got));
        }
        close(fd);
    }

    struct HomeIncludes final: public SshIncludes {
        HomeIncludes(StringView sshDirectory_, ObjPool& pool_, Vector<SshHost>& out_, int depth_)
            : sshDirectory(sshDirectory_)
            , pool(pool_)
            , out(out_)
            , depth(depth_)
        {
        }

        void include(StringView name) override {
            if (depth >= 8 || name.empty()) {
                return;
            }
            StringBuilder full;
            if (name.data()[0] == '/') {
                full << name;
            } else if (name.length() >= 2 && name.data()[0] == '~' && name.data()[1] == '/') {
                // ~/x: the directory above .ssh is home.
                full << sshDirectory << StringView(u8"/../") << StringView(name.data() + 2, name.length() - 2);
            } else {
                full << sshDirectory << StringView(u8"/") << name;
            }
            glob_t found{};
            if (glob(full.cStr(), 0, nullptr, &found) == 0) {
                for (size_t at = 0; at < found.gl_pathc; ++at) {
                    Buffer text;
                    readFile(found.gl_pathv[at], text);
                    HomeIncludes deeper(sshDirectory, pool, out, depth + 1);
                    sshConfigHosts(StringView(text), pool, out, &deeper);
                }
            }
            globfree(&found);
        }

        StringView sshDirectory;
        ObjPool& pool;
        Vector<SshHost>& out;
        int depth;
    };
}

void sshConfigHosts(StringView text, ObjPool& pool, Vector<SshHost>& out, SshIncludes* includes) {
    // The hosts the current block names, as indexes into `out`.
    Vector<size_t> block;
    Vector<StringView> args;
    const u8* p = (const u8*)(text.data());
    const u8* const end = p + text.length();
    while (p < end) {
        const u8* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n') {
            ++lineEnd;
        }
        const u8* q = p;
        p = lineEnd < end ? lineEnd + 1 : end;
        while (q < lineEnd && space(*q)) {
            ++q;
        }
        if (q >= lineEnd || *q == '#') {
            continue;
        }
        const u8* const keyStart = q;
        while (q < lineEnd && !space(*q) && *q != '=') {
            ++q;
        }
        const StringView key(keyStart, (size_t)(q - keyStart));
        while (q < lineEnd && (space(*q) || *q == '=')) {
            ++q;
        }
        words(StringView(q, (size_t)(lineEnd - q)), args);
        if (keyIs(key, "host")) {
            block.clear();
            for (const StringView name : args) {
                if (pattern(name)) {
                    continue;
                }
                size_t index = out.length();
                for (size_t at = 0; at < out.length(); ++at) {
                    if (out[at].alias == name) {
                        index = at;
                        break;
                    }
                }
                if (index == out.length()) {
                    SshHost host;
                    host.alias = pool.intern(name);
                    out.pushBack(host);
                }
                block.pushBack(index);
            }
        } else if (keyIs(key, "match")) {
            block.clear();
        } else if (keyIs(key, "include")) {
            if (includes != nullptr) {
                for (const StringView name : args) {
                    includes->include(name);
                }
            }
        } else if (!args.empty()) {
            StringView SshHost::* field = nullptr;
            if (keyIs(key, "hostname")) {
                field = &SshHost::hostName;
            } else if (keyIs(key, "user")) {
                field = &SshHost::user;
            } else if (keyIs(key, "port")) {
                field = &SshHost::port;
            }
            if (field != nullptr) {
                for (const size_t index : block) {
                    if ((out.mut(index).*field).empty()) {
                        out.mut(index).*field = pool.intern(args[0]);
                    }
                }
            }
        }
    }
}

void sshConfigHostsFromHome(StringView home, ObjPool& pool, Vector<SshHost>& out) {
    if (home.empty()) {
        return;
    }
    StringBuilder directory;
    directory << home << StringView(u8"/.ssh");
    StringBuilder path;
    path << StringView(directory) << StringView(u8"/config");
    Buffer text;
    readFile(path.cStr(), text);
    HomeIncludes includes(StringView(directory), pool, out, 1);
    sshConfigHosts(StringView(text), pool, out, &includes);
}

namespace {
    // Just enough JSON for tsh's output: values are walked, strings kept,
    // and the three fields a node row needs picked out on the way.
    struct Json {
        const u8* p;
        const u8* end;
        ObjPool& pool;
        bool broken = false;

        void skip() {
            while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
                ++p;
            }
        }

        bool eat(u8 c) {
            skip();
            if (p < end && *p == c) {
                ++p;
                return true;
            }
            return false;
        }

        StringView string() {
            skip();
            if (p >= end || *p != '"') {
                broken = true;
                return StringView();
            }
            ++p;
            StringBuilder text;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                    ++p;
                    const u8 c = *p++;
                    switch (c) {
                        case 'n':
                            text << StringView(u8"\n");
                            break;
                        case 't':
                            text << StringView(u8"\t");
                            break;
                        case 'u':
                            // Outside ASCII nothing here needs to be exact:
                            // a placeholder keeps the length sane.
                            p = p + 4 <= end ? p + 4 : end;
                            text << StringView(u8"?");
                            break;
                        default:
                            text << StringView(&c, 1);
                    }
                    continue;
                }
                text << StringView(p, 1);
                ++p;
            }
            if (p >= end) {
                broken = true;
                return StringView();
            }
            ++p;
            return pool.intern(StringView(text));
        }

        // Skips any value.
        void value() {
            skip();
            if (p >= end) {
                broken = true;
                return;
            }
            if (*p == '"') {
                string();
            } else if (*p == '{') {
                ++p;
                if (eat('}')) {
                    return;
                }
                do {
                    string();
                    if (!eat(':')) {
                        broken = true;
                        return;
                    }
                    value();
                } while (!broken && eat(','));
                if (!eat('}')) {
                    broken = true;
                }
            } else if (*p == '[') {
                ++p;
                if (eat(']')) {
                    return;
                }
                do {
                    value();
                } while (!broken && eat(','));
                if (!eat(']')) {
                    broken = true;
                }
            } else {
                while (p < end && *p != ',' && *p != '}' && *p != ']' && !space(*p) && *p != '\n') {
                    ++p;
                }
            }
        }

        // An object, each member handed to `member` with the parser at its
        // value; `member` must consume it.
        template <class F>
        void object(F&& member) {
            if (!eat('{')) {
                broken = true;
                return;
            }
            if (eat('}')) {
                return;
            }
            do {
                const StringView key = string();
                if (broken || !eat(':')) {
                    broken = true;
                    return;
                }
                member(key);
            } while (!broken && eat(','));
            if (!eat('}')) {
                broken = true;
            }
        }
    };
}

void teleportHosts(StringView json, ObjPool& pool, Vector<TeleportHost>& out) {
    Json in{(const u8*)(json.data()), (const u8*)(json.data()) + json.length(), pool};
    if (!in.eat('[')) {
        return;
    }
    if (in.eat(']')) {
        return;
    }
    Vector<TeleportHost> found;
    do {
        TeleportHost host;
        StringBuilder labels;
        in.object([&](StringView key) {
            if (key == StringView(u8"spec")) {
                in.object([&](StringView field) {
                    if (field == StringView(u8"hostname")) {
                        host.hostname = in.string();
                    } else if (field == StringView(u8"addr")) {
                        host.address = in.string();
                    } else {
                        in.value();
                    }
                });
            } else if (key == StringView(u8"metadata")) {
                in.object([&](StringView field) {
                    if (field == StringView(u8"labels")) {
                        in.object([&](StringView label) {
                            const StringView value = in.string();
                            if (!labels.empty()) {
                                labels << StringView(u8" · ");
                            }
                            labels << label << StringView(u8"=") << value;
                        });
                    } else {
                        in.value();
                    }
                });
            } else {
                in.value();
            }
        });
        if (in.broken) {
            return;
        }
        if (!host.hostname.empty()) {
            host.labels = pool.intern(StringView(labels));
            found.pushBack(host);
        }
    } while (in.eat(','));
    if (!in.eat(']')) {
        return;
    }
    for (const TeleportHost& host : found) {
        out.pushBack(host);
    }
}
