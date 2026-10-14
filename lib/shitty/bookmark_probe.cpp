/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "bookmark_probe.h"

#include "bookmarks.h"
#include "composer.h"

#include <lib/vterm/listener.h>

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>

#include <plt/loop_wake.h>
#include <plt/platform.h>
#include <plt/poller.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

using namespace stl;

namespace {
    // Options of ssh(1) that take a value, the letters of its
    // "bBcDEeFIiJLlmOoPpQRSWw" synopsis.
    bool takesValue(char option) {
        return strchr("bBcDEeFIiJLlmOoPpQRSWw", option) != nullptr;
    }

    // The next shell word: blanks skipped, single and double quotes and
    // backslashes undone - enough for the command lines a bookmark holds.
    bool nextWord(StringView& rest, StringBuilder& out) {
        out.reset();
        size_t at = 0;
        while (at < rest.length() && (rest[at] == ' ' || rest[at] == '\t')) {
            ++at;
        }
        if (at == rest.length()) {
            rest = StringView();
            return false;
        }
        char quote = 0;
        for (; at < rest.length(); ++at) {
            const char c = (char)(rest[at]);
            if (quote != 0) {
                if (c == quote) {
                    quote = 0;
                } else if (quote == '"' && c == '\\' && at + 1 < rest.length()) {
                    ++at;
                    out << StringView(rest.data() + at, 1);
                } else {
                    out << StringView(rest.data() + at, 1);
                }
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '\\' && at + 1 < rest.length()) {
                ++at;
                out << StringView(rest.data() + at, 1);
            } else if (c == ' ' || c == '\t') {
                break;
            } else {
                out << StringView(rest.data() + at, 1);
            }
        }
        rest = StringView(rest.data() + at, rest.length() - at);
        return true;
    }

    void assign(StringBuilder& out, StringView text) {
        out.reset();
        out << text;
    }
}

bool sshTarget(StringView command, StringBuilder& host, StringBuilder& port) {
    host.reset();
    port.reset();
    StringView rest = command;
    StringBuilder word;
    if (!nextWord(rest, word)) {
        return false;
    }
    StringView program(word);
    for (size_t at = program.length(); at > 0; --at) {
        if (program[at - 1] == '/') {
            program = StringView(program.data() + at, program.length() - at);
            break;
        }
    }
    if (program != StringView(u8"ssh")) {
        return false;
    }
    while (nextWord(rest, word)) {
        const StringView text(word);
        if (text == StringView(u8"--")) {
            if (!nextWord(rest, word)) {
                return false;
            }
            break;
        }
        if (text.length() >= 2 && text[0] == '-') {
            // A cluster like -At, or an option with its value attached
            // (-p2222) or in the next word (-p 2222).
            for (size_t at = 1; at < text.length(); ++at) {
                const char option = (char)(text[at]);
                if (!takesValue(option)) {
                    continue;
                }
                StringView value(text.data() + at + 1, text.length() - at - 1);
                StringBuilder next;
                if (value.empty()) {
                    if (!nextWord(rest, next)) {
                        return false;
                    }
                    value = StringView(next);
                }
                if (option == 'p') {
                    assign(port, value);
                }
                break;
            }
            continue;
        }
        break;
    }
    StringView destination(word);
    if (destination.empty() || destination[0] == '-') {
        return false;
    }
    const bool url = destination.startsWith(StringView(u8"ssh://"));
    if (url) {
        destination = StringView(destination.data() + 6, destination.length() - 6);
    }
    // The user goes: everything up to the last @.
    for (size_t at = destination.length(); at > 0; --at) {
        if (destination[at - 1] == '@') {
            destination = StringView(destination.data() + at, destination.length() - at);
            break;
        }
    }
    if (url) {
        // ssh://host:port, and a trailing path is not ssh's to have.
        for (size_t at = 0; at < destination.length(); ++at) {
            if (destination[at] == '/') {
                destination = StringView(destination.data(), at);
                break;
            }
        }
        for (size_t at = 0; at < destination.length(); ++at) {
            if (destination[at] == ':') {
                assign(port, StringView(destination.data() + at + 1, destination.length() - at - 1));
                destination = StringView(destination.data(), at);
                break;
            }
        }
    }
    if (destination.empty()) {
        return false;
    }
    assign(host, destination);
    return true;
}

bool sshConfigTarget(StringView config, StringView alias, StringBuilder& host, StringBuilder& port) {
    StringView rest = config;
    // Inside a Host block that names the alias; once one has been left,
    // the first answer stands, as ssh takes the first value it finds.
    bool inside = false;
    // In `Host *`, whose proxy settings reach every host.
    bool everyHost = false;
    bool direct = true;
    bool hostSet = false;
    bool portSet = false;
    StringBuilder word;
    while (!rest.empty()) {
        StringView line;
        StringView after;
        if (rest.split('\n', line, after)) {
            rest = after;
        } else {
            line = rest;
            rest = StringView();
        }
        line = line.stripCr().stripSpace();
        if (line.empty() || line[0] == '#') {
            continue;
        }
        // Keyword, then its value after blanks or an `=`.
        size_t cut = 0;
        while (cut < line.length() && line[cut] != ' ' && line[cut] != '\t' && line[cut] != '=') {
            ++cut;
        }
        StringBuilder keyword;
        for (size_t at = 0; at < cut; ++at) {
            const char c = (char)(line[at]);
            const char lower = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            keyword << StringView((const u8*)(&lower), 1);
        }
        StringView value(line.data() + cut, line.length() - cut);
        value = value.stripSpace();
        if (!value.empty() && value[0] == '=') {
            value = StringView(value.data() + 1, value.length() - 1).stripSpace();
        }
        const StringView key(keyword);
        if (key == StringView(u8"host") || key == StringView(u8"match")) {
            inside = false;
            everyHost = false;
            if (key == StringView(u8"host")) {
                StringView names = value;
                while (nextWord(names, word)) {
                    if (StringView(word) == alias) {
                        inside = true;
                    } else if (StringView(word) == StringView(u8"*")) {
                        everyHost = true;
                    }
                }
            }
            continue;
        }
        if ((inside || everyHost) && (key == StringView(u8"proxyjump") || key == StringView(u8"proxycommand")) && value != StringView(u8"none")) {
            direct = false;
        }
        if (!inside) {
            continue;
        }
        if (key == StringView(u8"hostname") && !hostSet) {
            StringView words = value;
            if (nextWord(words, word)) {
                assign(host, StringView(word));
                hostSet = true;
            }
        } else if (key == StringView(u8"port") && !portSet) {
            StringView words = value;
            if (nextWord(words, word)) {
                assign(port, StringView(word));
                portSet = true;
            }
        }
    }
    return direct;
}

bool hostAnswers(const char* host, const char* port, int timeoutMs) {
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host, port, &hints, &found) != 0 || found == nullptr) {
        return false;
    }
    bool answered = false;
    for (addrinfo* address = found; address != nullptr && !answered; address = address->ai_next) {
        const int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int result = connect(fd, address->ai_addr, address->ai_addrlen);
        if (result == 0) {
            answered = true;
        } else if (errno == EINPROGRESS) {
            pollfd waiting{fd, POLLOUT, 0};
            if (poll(&waiting, 1, timeoutMs) == 1) {
                int error = 0;
                socklen_t length = sizeof(error);
                answered = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
            }
        }
        close(fd);
    }
    freeaddrinfo(found);
    return answered;
}

namespace {
    constexpr size_t probeTargets = 64;
    constexpr int connectTimeoutMs = 3000;

    struct Target {
        u64 id = 0;
        char host[256] = {};
        char port[16] = {};
        // Answers of the last round.
        bool asked = false;
        bool unreachable = false;
    };

    void copyInto(char* out, size_t size, StringView text) {
        const size_t length = text.length() < size - 1 ? text.length() : size - 1;
        memcpy(out, text.data(), length);
        out[length] = '\0';
    }

    // ~/.ssh/config as it is now; empty when there is none.
    void readSshConfig(StringBuilder& out) {
        out.reset();
        const char* const home = getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            return;
        }
        StringBuilder path;
        path << StringView(home) << StringView(u8"/.ssh/config");
        const int fd = open(path.cStr(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        char chunk[4096];
        for (;;) {
            const ssize_t got = read(fd, chunk, sizeof(chunk));
            if (got <= 0) {
                break;
            }
            out << StringView((const u8*)(chunk), (size_t)(got));
        }
        close(fd);
    }

    struct BookmarkProbeImpl final: public BookmarkProbe, public plt::TimerCallback {
        explicit BookmarkProbeImpl(Composer& composer_)
            : composer(composer_)
        {
            pthread_mutex_init(&mutex, nullptr);
            pthread_cond_init(&changed, nullptr);
        }

        void watch(const BookmarkShelf& shelf) override {
            pthread_mutex_lock(&mutex);
            Target next[probeTargets];
            size_t count = 0;
            StringBuilder host;
            StringBuilder port;
            for (const Bookmark& bookmark : shelf.items) {
                if (count == probeTargets || !sshTarget(bookmark.command, host, port)) {
                    continue;
                }
                Target& target = next[count++];
                target.id = bookmark.id;
                copyInto(target.host, sizeof(target.host), StringView(host));
                copyInto(target.port, sizeof(target.port), StringView(port));
                // A host that was already being asked keeps its answer
                // until the next round replaces it.
                for (size_t old = 0; old < targetCount; ++old) {
                    if (targets[old].id == target.id && strcmp(targets[old].host, target.host) == 0 && strcmp(targets[old].port, target.port) == 0) {
                        target.asked = targets[old].asked;
                        target.unreachable = targets[old].unreachable;
                    }
                }
            }
            for (size_t at = 0; at < count; ++at) {
                targets[at] = next[at];
            }
            targetCount = count;
            pending = true;
            pthread_cond_signal(&changed);
            const bool start = !started && count != 0;
            started = started || start;
            pthread_mutex_unlock(&mutex);
            if (start) {
                doorbell = composer.platform->createLoopWake(*composer.pool, *this);
                pthread_t thread;
                if (pthread_create(&thread, nullptr, run, this) == 0) {
                    pthread_detach(thread);
                }
            }
        }

        bool unreachable(u64 bookmark) const override {
            pthread_mutex_lock(&mutex);
            bool answer = false;
            for (size_t at = 0; at < targetCount; ++at) {
                if (targets[at].id == bookmark) {
                    answer = targets[at].asked && targets[at].unreachable;
                }
            }
            pthread_mutex_unlock(&mutex);
            return answer;
        }

        // Main thread, woken by the probe thread: the chrome hears it the
        // way it hears any change to the tabs.
        void ready() override {
            for (IntrusiveNode* node = composer.sessionsChangedListeners.mutFront(); node != composer.sessionsChangedListeners.mutEnd();) {
                Listener* const listener = static_cast<Listener*>(node);
                node = node->next;
                listener->onListen();
            }
        }

        static void* run(void* self) {
            static_cast<BookmarkProbeImpl*>(self)->loop();
            return nullptr;
        }

        void loop() {
            Target round[probeTargets];
            StringBuilder output;
            StringBuilder host;
            StringBuilder port;
            for (;;) {
                pthread_mutex_lock(&mutex);
                while (!pending) {
                    timespec until;
                    clock_gettime(CLOCK_REALTIME, &until);
                    until.tv_sec += bookmarkProbeIntervalSeconds;
                    if (pthread_cond_timedwait(&changed, &mutex, &until) == ETIMEDOUT) {
                        break;
                    }
                }
                pending = false;
                const size_t count = targetCount;
                for (size_t at = 0; at < count; ++at) {
                    round[at] = targets[at];
                }
                pthread_mutex_unlock(&mutex);

                bool moved = false;
                for (size_t at = 0; at < count; ++at) {
                    assign(host, StringView(round[at].host));
                    assign(port, StringView(round[at].port));
                    readSshConfig(output);
                    if (!sshConfigTarget(StringView(output), StringView(round[at].host), host, port)) {
                        // Behind a proxy: nothing a direct connection
                        // could tell, so no answer at all.
                        continue;
                    }
                    if (StringView(port).empty()) {
                        assign(port, StringView(u8"22"));
                    }
                    const bool answers = hostAnswers(host.cStr(), port.cStr(), connectTimeoutMs);
                    pthread_mutex_lock(&mutex);
                    for (size_t live = 0; live < targetCount; ++live) {
                        Target& target = targets[live];
                        if (target.id == round[at].id && strcmp(target.host, round[at].host) == 0) {
                            moved = moved || !target.asked || target.unreachable == answers;
                            target.asked = true;
                            target.unreachable = !answers;
                        }
                    }
                    pthread_mutex_unlock(&mutex);
                }
                if (moved && doorbell != nullptr) {
                    doorbell->signal();
                }
            }
        }

        Composer& composer;
        mutable pthread_mutex_t mutex;
        pthread_cond_t changed;
        Target targets[probeTargets];
        size_t targetCount = 0;
        bool pending = false;
        bool started = false;
        plt::LoopWake* doorbell = nullptr;
    };
}

BookmarkProbe* BookmarkProbe::create(ObjPool& owner, Composer& composer) {
    return owner.make<BookmarkProbeImpl>(composer);
}
