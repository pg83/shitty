/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <plt/input.h>

#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/str/view.h>

#include <stddef.h>

// A name being typed into a row of the list pt draws on Wayland - a
// folder's, a tab's, a bookmark's - in place, as Finder renames a file:
// the old name selected, typing replaces it, Enter keeps it, Escape puts
// the old one back. Text, caret and selection only; the drawing is the
// list's.
class SidebarField {
public:
    enum class Outcome : u8 {
        // Not a key the field takes.
        Ignored,
        Edited,
        Commit,
        Cancel
    };

    // Starts with `text`, all of it selected.
    void begin(stl::StringView text);
    // The text as UTF-8; valid until the next edit.
    stl::StringView text();
    // Codepoints, the caret and the selection's other end, in codepoints.
    size_t length() const {
        return text_.length();
    }
    size_t caret() const {
        return caret_;
    }
    size_t anchor() const {
        return anchor_;
    }
    bool selected() const {
        return caret_ != anchor_;
    }
    // Codepoints [from, to) as UTF-8, appended: where the list measures the
    // caret and the selection from.
    void utf8(size_t from, size_t to, stl::StringBuilder& out) const;

    // Typed text replaces the selection.
    void insert(u32 codepoint);
    Outcome key(const plt::KeyInput& key);

private:
    void erase(size_t from, size_t to);
    void move(size_t to, bool extend);

    stl::Vector<u32> text_;
    stl::StringBuilder utf8_;
    size_t caret_ = 0;
    size_t anchor_ = 0;
};
