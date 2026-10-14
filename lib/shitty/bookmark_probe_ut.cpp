/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "bookmark_probe.h"

#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/tst/ut.h>

#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace stl;

namespace {
    bool target(const char* command, const char* host, const char* port) {
        StringBuilder gotHost;
        StringBuilder gotPort;
        if (!sshTarget(StringView(command), gotHost, gotPort)) {
            return false;
        }
        return StringView(gotHost) == StringView(host) && StringView(gotPort) == StringView(port);
    }

    // A listening socket on the loopback, its port in `port`.
    int listenLocally(char port[16]) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        STD_INSIST(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        STD_INSIST(bind(fd, (sockaddr*)(&address), sizeof(address)) == 0);
        STD_INSIST(listen(fd, 4) == 0);
        socklen_t length = sizeof(address);
        STD_INSIST(getsockname(fd, (sockaddr*)(&address), &length) == 0);
        snprintf(port, 16, "%u", (unsigned)(ntohs(address.sin_port)));
        return fd;
    }
}

STD_TEST_SUITE(BookmarkProbe) {
    STD_TEST(TheDestinationIsReadTheWaySshReadsItsArguments) {
        STD_INSIST(target("ssh prod", "prod", ""));
        STD_INSIST(target("ssh user@prod.example", "prod.example", ""));
        STD_INSIST(target("/usr/bin/ssh -p 2222 prod", "prod", "2222"));
        STD_INSIST(target("ssh -p2222 -A prod uptime", "prod", "2222"));
        // A value-taking option's value is not the destination.
        STD_INSIST(target("ssh -i ~/.ssh/key -o StrictHostKeyChecking=no -J bastion prod", "prod", ""));
        STD_INSIST(target("ssh -At prod", "prod", ""));
        STD_INSIST(target("ssh 'me@db 1'", "db 1", ""));
        STD_INSIST(target("ssh ssh://me@db.example:2200/x", "db.example", "2200"));
        STD_INSIST(target("ssh -- prod", "prod", ""));
    }

    STD_TEST(WhatIsNotAnSshCommandHasNoHost) {
        StringBuilder host;
        StringBuilder port;
        STD_INSIST(!sshTarget(StringView(u8"mosh prod"), host, port));
        STD_INSIST(!sshTarget(StringView(u8"sshfs prod:/ /mnt"), host, port));
        STD_INSIST(!sshTarget(StringView(u8"ssh"), host, port));
        STD_INSIST(!sshTarget(StringView(u8"ssh -p"), host, port));
        STD_INSIST(!sshTarget(StringView(), host, port));
    }

    // The alias's own block answers; another host's block and a later
    // block for the same alias do not.
    STD_TEST(AnAliasIsLookedUpInTheSshConfig) {
        const StringView config(u8"Host other\n"
                                "  HostName 10.0.0.9\n"
                                "  Port 2200\n"
                                "\n"
                                "Host prod prod-alt\n"
                                "    hostname=10.0.0.1\n"
                                "    PORT 2222\n"
                                "Host prod\n"
                                "    HostName 10.0.0.2\n");
        StringBuilder host;
        StringBuilder port;
        host << StringView(u8"prod");
        STD_INSIST(sshConfigTarget(config, StringView(u8"prod"), host, port));
        STD_INSIST(StringView(host) == StringView(u8"10.0.0.1"));
        STD_INSIST(StringView(port) == StringView(u8"2222"));

        StringBuilder plainHost;
        StringBuilder plainPort;
        plainHost << StringView(u8"elsewhere");
        STD_INSIST(sshConfigTarget(config, StringView(u8"elsewhere"), plainHost, plainPort));
        STD_INSIST(StringView(plainHost) == StringView(u8"elsewhere"));
        STD_INSIST(StringView(plainPort).empty());
    }

    // A host behind a bastion is not ssh's to reach directly, so it is not
    // probed: its own block, or every host's.
    STD_TEST(AHostBehindAProxyIsNotProbed) {
        StringBuilder host;
        StringBuilder port;
        STD_INSIST(!sshConfigTarget(StringView(u8"Host prod\n  ProxyJump bastion\n"), StringView(u8"prod"), host, port));
        STD_INSIST(!sshConfigTarget(StringView(u8"Host *\n  ProxyCommand nc %h %p\n"), StringView(u8"prod"), host, port));
        STD_INSIST(sshConfigTarget(StringView(u8"Host prod\n  ProxyJump none\n"), StringView(u8"prod"), host, port));
        STD_INSIST(sshConfigTarget(StringView(u8"Host other\n  ProxyJump bastion\n"), StringView(u8"prod"), host, port));
    }

    // A listening port answers; the same port once closed does not.
    STD_TEST(AHostAnswersWhenSomethingAcceptsTheConnection) {
        char port[16];
        const int fd = listenLocally(port);
        STD_INSIST(hostAnswers("127.0.0.1", port, 1000));
        close(fd);
        STD_INSIST(!hostAnswers("127.0.0.1", port, 1000));
    }
}
