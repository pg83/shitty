/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/lib/vector.h>
#include <std/str/view.h>

#include <stddef.h>

// How well a typed query matches a name in the command palette: every
// character of the query, in order, somewhere in the name, ASCII letters
// without regard to case. More for a match at the start of a word (after
// / - _ . space @ :) and for runs of characters in a row, less for the
// characters skipped before it starts. -1 when the query is not in the
// name at all; an empty query matches everything with 0.
//
// `positions`, when given, gets the byte offsets in `text` of the matched
// characters, for drawing them highlighted. Replaces what it held.
int fuzzyScore(stl::StringView query, stl::StringView text, stl::Vector<size_t>* positions = nullptr);
