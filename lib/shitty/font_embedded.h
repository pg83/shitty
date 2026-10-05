/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

struct Composer;
struct FontResolver;

// Serves the fonts compiled into the binary: resolves any regular request
// to the embedded mono face as the last resort and contributes the emoji
// and mono faces as implicit coverage fallbacks.
FontResolver* createEmbeddedFontResolver(Composer& composer);

// The embedded mono face's bytes, for text drawn outside the terminal's
// grid (ui_text.h) where no system face is found.
void embeddedMonoFont(const void*& data, unsigned long& size);
