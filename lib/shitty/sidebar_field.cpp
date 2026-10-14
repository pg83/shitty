/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "sidebar_field.h"

using namespace stl;

namespace {
    void appendUtf8(u32 codepoint, StringBuilder& out) {
        char bytes[4];
        size_t count = 0;
        if (codepoint < 0x80) {
            bytes[count++] = (char)(codepoint);
        } else if (codepoint < 0x800) {
            bytes[count++] = (char)(0xc0 | (codepoint >> 6));
            bytes[count++] = (char)(0x80 | (codepoint & 0x3f));
        } else if (codepoint < 0x10000) {
            bytes[count++] = (char)(0xe0 | (codepoint >> 12));
            bytes[count++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
            bytes[count++] = (char)(0x80 | (codepoint & 0x3f));
        } else {
            bytes[count++] = (char)(0xf0 | (codepoint >> 18));
            bytes[count++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
            bytes[count++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
            bytes[count++] = (char)(0x80 | (codepoint & 0x3f));
        }
        out << StringView((const u8*)(bytes), count);
    }
}

void SidebarField::begin(StringView text) {
    text_.clear();
    const u8* at = (const u8*)(text.data());
    const u8* const end = at + text.length();
    while (at < end) {
        u32 codepoint = *at;
        size_t extra = 0;
        if (codepoint >= 0xf0) {
            codepoint &= 0x07;
            extra = 3;
        } else if (codepoint >= 0xe0) {
            codepoint &= 0x0f;
            extra = 2;
        } else if (codepoint >= 0xc0) {
            codepoint &= 0x1f;
            extra = 1;
        }
        ++at;
        for (; extra > 0 && at < end; --extra, ++at) {
            codepoint = (codepoint << 6) | (*at & 0x3f);
        }
        text_.pushBack(codepoint);
    }
    anchor_ = 0;
    caret_ = text_.length();
}

StringView SidebarField::text() {
    utf8_.reset();
    utf8(0, text_.length(), utf8_);
    return StringView(utf8_);
}

void SidebarField::utf8(size_t from, size_t to, StringBuilder& out) const {
    for (size_t at = from; at < to && at < text_.length(); ++at) {
        appendUtf8(text_[at], out);
    }
}

void SidebarField::erase(size_t from, size_t to) {
    if (to > text_.length()) {
        to = text_.length();
    }
    if (from >= to) {
        return;
    }
    const size_t count = to - from;
    for (size_t at = from; at + count < text_.length(); ++at) {
        text_.mut(at) = text_[at + count];
    }
    for (size_t gone = 0; gone < count; ++gone) {
        text_.popBack();
    }
    caret_ = from;
    anchor_ = from;
}

void SidebarField::move(size_t to, bool extend) {
    caret_ = to;
    if (!extend) {
        anchor_ = to;
    }
}

void SidebarField::insert(u32 codepoint) {
    if (codepoint < 0x20 || codepoint == 0x7f) {
        return;
    }
    erase(caret_ < anchor_ ? caret_ : anchor_, caret_ < anchor_ ? anchor_ : caret_);
    text_.pushBack(0);
    for (size_t at = text_.length() - 1; at > caret_; --at) {
        text_.mut(at) = text_[at - 1];
    }
    text_.mut(caret_) = codepoint;
    ++caret_;
    anchor_ = caret_;
}

SidebarField::Outcome SidebarField::key(const plt::KeyInput& key) {
    const bool extend = (key.modifiers & plt::InputShift) != 0;
    const size_t low = caret_ < anchor_ ? caret_ : anchor_;
    const size_t high = caret_ < anchor_ ? anchor_ : caret_;
    switch (key.key) {
        case plt::InputKey::Enter:
            return Outcome::Commit;
        case plt::InputKey::Escape:
            return Outcome::Cancel;
        case plt::InputKey::Backspace:
            if (selected()) {
                erase(low, high);
            } else if (caret_ > 0) {
                erase(caret_ - 1, caret_);
            }
            return Outcome::Edited;
        case plt::InputKey::Delete:
            if (selected()) {
                erase(low, high);
            } else {
                erase(caret_, caret_ + 1);
            }
            return Outcome::Edited;
        case plt::InputKey::Left:
            // A selection collapses to its start, as in any text field.
            move(selected() && !extend ? low : caret_ > 0 ? caret_ - 1 : 0, extend);
            return Outcome::Edited;
        case plt::InputKey::Right:
            move(selected() && !extend ? high : caret_ < text_.length() ? caret_ + 1 : caret_, extend);
            return Outcome::Edited;
        case plt::InputKey::Home:
            move(0, extend);
            return Outcome::Edited;
        case plt::InputKey::End:
            move(text_.length(), extend);
            return Outcome::Edited;
        default:
            break;
    }
    // Ctrl+A selects it all, as Cmd+A does on a Mac.
    if ((key.modifiers & plt::InputControl) != 0 && (key.baseCodepoint == 'a' || key.layoutCodepoint == 'a')) {
        anchor_ = 0;
        caret_ = text_.length();
        return Outcome::Edited;
    }
    return Outcome::Ignored;
}
