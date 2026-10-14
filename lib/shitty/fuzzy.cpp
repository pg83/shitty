/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "fuzzy.h"

using namespace stl;

namespace {
    constexpr int matchPoint = 16;
    constexpr int wordStart = 24;
    constexpr int textStart = 32;
    constexpr int runBonus = 20;
    constexpr int gapPenalty = 1;
    constexpr int leadPenalty = 2;
    constexpr int leadPenaltyMax = 12;

    u8 fold(u8 c) {
        return c >= 'A' && c <= 'Z' ? (u8)(c - 'A' + 'a') : c;
    }

    bool boundary(u8 c) {
        return c == '/' || c == '-' || c == '_' || c == '.' || c == ' ' || c == '@' || c == ':';
    }

    // The score of the match whose first character is at `start`, each
    // later character taken as early as it comes; -1 when the rest does
    // not fit.
    int matchFrom(const u8* q, size_t ql, const u8* t, size_t tl, size_t start, Vector<size_t>* positions) {
        if (positions != nullptr) {
            positions->clear();
        }
        int score = 0;
        size_t at = start;
        size_t previous = (size_t)(-1);
        for (size_t i = 0; i < ql; ++i) {
            while (at < tl && fold(t[at]) != fold(q[i])) {
                ++at;
            }
            if (at >= tl) {
                return -1;
            }
            score += matchPoint;
            if (at == 0) {
                score += textStart;
            } else if (boundary(t[at - 1])) {
                score += wordStart;
            }
            if (previous != (size_t)(-1)) {
                if (at == previous + 1) {
                    score += runBonus;
                } else {
                    score -= (int)(at - previous - 1) * gapPenalty;
                }
            }
            if (positions != nullptr) {
                positions->pushBack(at);
            }
            previous = at;
            ++at;
        }
        const int lead = (int)(start) * leadPenalty;
        score -= lead < leadPenaltyMax ? lead : leadPenaltyMax;
        return score;
    }
}

int fuzzyScore(StringView query, StringView text, Vector<size_t>* positions) {
    if (positions != nullptr) {
        positions->clear();
    }
    const u8* const q = (const u8*)(query.data());
    const u8* const t = (const u8*)(text.data());
    const size_t ql = query.length();
    const size_t tl = text.length();
    if (ql == 0) {
        return 0;
    }
    // Every place the first character occurs is tried as the start: the
    // first one is not always the best ("pr" in "api-proxy" wants the
    // second p).
    int best = -1;
    size_t bestStart = 0;
    for (size_t start = 0; start < tl; ++start) {
        if (fold(t[start]) != fold(q[0])) {
            continue;
        }
        const int score = matchFrom(q, ql, t, tl, start, nullptr);
        if (score < 0) {
            break;
        }
        if (score > best) {
            best = score;
            bestStart = start;
        }
    }
    if (best >= 0 && positions != nullptr) {
        matchFrom(q, ql, t, tl, bestStart, positions);
    }
    return best;
}
