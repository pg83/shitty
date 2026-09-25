/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "session.h"

#include "pty.h"
#include "brand.h"
#include "options.h"
#include "composer.h"
#include "debug_trace.h"
#include "input_bindings.h"

#include <lib/vterm/vterm.h>
#include <lib/vterm/listener.h>
#include <lib/vterm/input_handler.h>

#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/sys/throw.h>
#include <std/ios/sys.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <stdio.h>
#include <plt/fiber.h>
#include <plt/poller.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/loop_wake.h>

using namespace stl;

namespace {
    struct SessionSetImpl;

    // How often the reaper re-checks a grave whose cells are still retained
    // by the renderer while the successor's full expose is in flight.
    constexpr u64 gravePollUs = 10'000;

    // One node per terminal action, owned by the set and living as long as
    // it does. They never move, so an action cannot end up pointing at a
    // terminal that has stopped being the active one.
    struct CallSessionAction final: public Listener {
        CallSessionAction(SessionSetImpl* parent, InputActions action);

        void onListen(void*) override;

        SessionSetImpl* parent;
        InputActions action;
    };

    // The set is the one resize listener however many sessions there are:
    // a per-terminal registration could never leave composer's lists when
    // its session died, and background sessions still have to track the
    // window to be right when they come back.
    struct CallSessionsResize final: public Listener {
        explicit CallSessionsResize(SessionSetImpl* parent);

        void onListen(void*) override;

        SessionSetImpl* parent;
    };

    struct CallSessionsLayoutChanged final: public Listener {
        explicit CallSessionsLayoutChanged(SessionSetImpl* parent);
        void onListen(void*) override;
        SessionSetImpl* parent;
    };

    struct CallSessionsFontChanged final: public Listener {
        explicit CallSessionsFontChanged(SessionSetImpl* parent);

        void onListen(void*) override;

        SessionSetImpl* parent;
    };

    struct CallSessionsConfigChanged final: public Listener {
        explicit CallSessionsConfigChanged(SessionSetImpl* parent);

        void onListen(void*) override;

        SessionSetImpl* parent;
    };

    struct CallTitleChanged final: public Listener {
        explicit CallTitleChanged(SessionSetImpl* parent);

        void onListen(void* argument) override;

        SessionSetImpl* parent;
    };

    struct ReapBody final: public Runable {
        explicit ReapBody(SessionSetImpl* parent);

        void run() override;

        SessionSetImpl* parent;
    };

    struct PtyReadBody final: public Runable {
        PtyReadBody(SessionSetImpl* parent, u64 sessionId, PtyHandle& handle, Vterm& terminal);

        void run() override;

        SessionSetImpl* parent;
        u64 sessionId;
        PtyHandle* handle;
        Vterm* terminal;
    };

    struct PtyEofReady final: public plt::TimerCallback {
        explicit PtyEofReady(SessionSetImpl* parent);

        void ready() override;

        SessionSetImpl* parent;
    };

    // The window's single input handler. A terminal never joins the
    // router's chain: membership would then be what selects the active
    // one, which is exactly the coupling this removes.
    struct SessionSetImpl final: public SessionSet, public InputHandler {
        explicit SessionSetImpl(Composer& composer);
        ~SessionSetImpl() noexcept;

        Vterm* activeTerminal() const override;
        size_t count() const override;
        size_t activeIndex() const override;
        StringView title(size_t index) const override;

        bool key(const plt::KeyInput& input) override;
        bool text(const plt::TextInput& input) override;
        bool pointerMotion(const plt::PointerMotionInput& input) override;
        bool pointerButton(const plt::PointerButtonInput& input) override;
        bool scroll(const plt::ScrollInput& input) override;
        void focus(bool focused) override;
        void pointerPresence(bool present) override;
        void flush() override;

        void everyTerminalResized();
        void layoutChanged();
        VtPointerPosition pointerPosition(int x, int y);
        void everyTerminalFontChanged();
        void everyTerminalConfigChanged();
        void titleChanged(const VtermTitleChanged& event);
        void publishWindowTitle(StringView title);
        void newSession() override;
        void activate(size_t index) override;
        bool activateNext();
        bool activatePrevious();
        bool selectOrdinal(size_t ordinal);
        bool close(size_t index) override;
        bool closeActive();
        void publishSessionsChanged();
        void runReaper();
        void reapReady();
        bool canReap(Vterm* terminal) const;
        void ptyEof(u64 sessionId);
        void closeEndedSessions();
        PtySize ptySize() const;

        struct Session {
            Vterm* terminal = nullptr;
            PtyHandle* handle = nullptr;
            u64 id = 0;
            // Everything the terminal is - the handle, fibers, screens -
            // dies when this arena does.
            stl::ObjPool* arena = nullptr;
            // The session's last published title, arena-owned so the
            // record stays trivially copyable when slots shift.
            stl::Buffer* title = nullptr;
        };

        struct Grave {
            stl::ObjPool* arena = nullptr;
            Vterm* terminal = nullptr;
        };

        Composer& composer;
        // Storage only: Vector has no erase and is not assignable, so
        // count_ is the live length and closing shifts the tail down.
        Vector<Session> sessions;
        size_t count_ = 0;
        size_t active_ = 0;
        // Closed sessions whose arena is not yet safe to drop; the reaper
        // fiber drains this. Same storage discipline as sessions.
        Vector<Grave> graves;
        size_t graveCount_ = 0;
        plt::Fiber* reaper_ = nullptr;
        Vector<u64> endedSessions;
        u64 nextSessionId_ = 1;
        PtyEofReady eofReady{this};
        plt::LoopWake* eofWake_ = nullptr;
        // Replayed into a session as it becomes active, so a terminal that
        // was not there when the window gained focus still learns of it.
        // A window is born focused until its system says otherwise, which
        // is also what the first activation used to assume.
        bool focused_ = true;
        bool pointerPresent_ = false;
        bool pointerKnown_ = false;
        int pointerX_ = 0;
        int pointerY_ = 0;
        CallSessionAction copyAction{this, InputActions::Copy};
        CallSessionAction pasteAction{this, InputActions::Paste};
        CallSessionAction pastePrimaryAction{this, InputActions::PastePrimary};
        CallSessionAction pageUpAction{this, InputActions::PageUp};
        CallSessionAction pageDownAction{this, InputActions::PageDown};
        CallSessionAction clearAction{this, InputActions::Clear};
        CallSessionsResize resizeAction{this};
        CallSessionsLayoutChanged layoutChangedAction{this};
        CallSessionsFontChanged fontChangedAction{this};
        CallSessionsConfigChanged configChangedAction{this};
        CallTitleChanged titleChangedAction{this};
        CallSessionAction newTabAction{this, InputActions::NewTab};
        CallSessionAction closeTabAction{this, InputActions::CloseTab};
        CallSessionAction prevTabAction{this, InputActions::PrevTab};
        CallSessionAction nextTabAction{this, InputActions::NextTab};
        CallSessionAction selectTabActions[9]{
            {this, InputActions::SelectTab1},
            {this, InputActions::SelectTab2},
            {this, InputActions::SelectTab3},
            {this, InputActions::SelectTab4},
            {this, InputActions::SelectTab5},
            {this, InputActions::SelectTab6},
            {this, InputActions::SelectTab7},
            {this, InputActions::SelectTab8},
            {this, InputActions::SelectTab9},
        };
        CallSessionAction wordLeftAction{this, InputActions::WordLeft};
        CallSessionAction wordRightAction{this, InputActions::WordRight};
        CallSessionAction lineStartAction{this, InputActions::LineStart};
        CallSessionAction lineEndAction{this, InputActions::LineEnd};
        CallSessionAction killLineAction{this, InputActions::KillLine};
        CallSessionAction eraseWordAction{this, InputActions::EraseWord};
        ReapBody reapBody{this};
    };
}

CallSessionAction::CallSessionAction(SessionSetImpl* parent_, InputActions action_)
    : parent(parent_)
    , action(action_)
{
}

CallSessionsResize::CallSessionsResize(SessionSetImpl* parent_)
    : parent(parent_)
{
}

void CallSessionsResize::onListen(void*) {
    parent->everyTerminalResized();
}

CallSessionsFontChanged::CallSessionsFontChanged(SessionSetImpl* parent_)
    : parent(parent_)
{
}

void CallSessionsFontChanged::onListen(void*) {
    parent->everyTerminalFontChanged();
}

CallSessionsConfigChanged::CallSessionsConfigChanged(SessionSetImpl* parent_)
    : parent(parent_)
{
}

void CallSessionsConfigChanged::onListen(void*) {
    parent->everyTerminalConfigChanged();
}

CallTitleChanged::CallTitleChanged(SessionSetImpl* parent_)
    : parent(parent_)
{
}

void CallTitleChanged::onListen(void* argument) {
    parent->titleChanged(*static_cast<VtermTitleChanged*>(argument));
}

ReapBody::ReapBody(SessionSetImpl* parent_)
    : parent(parent_)
{
}

PtyReadBody::PtyReadBody(SessionSetImpl* parent_, u64 sessionId_, PtyHandle& handle_, Vterm& terminal_)
    : parent(parent_)
    , sessionId(sessionId_)
    , handle(&handle_)
    , terminal(&terminal_)
{
}

void PtyReadBody::run() {
    // A batch of drain blocks lands in the parser under one bookkeeping
    // wrap; the yield keeps frames and input interleaved with a flooding
    // child while the drain thread keeps the kernel busy.
    constexpr size_t batchLimit = 32;
    constexpr size_t sliceSize = 256 * 1024;
    StringView slices[batchLimit];
    size_t inSlice = 0;
    for (;;) {
        PtyHandle::Chunk* const chunks = handle->acquire();
        if (chunks == nullptr) {
            parent->ptyEof(sessionId);
            return;
        }
        PtyHandle::Chunk* chunk = chunks;
        while (chunk != nullptr) {
            size_t count = 0;
            while (chunk != nullptr && count < batchLimit) {
                slices[count] = chunk->chunk();
                inSlice += slices[count].length();
                ++count;
                chunk = chunk->next();
            }
            terminal->feedPty(slices, count);
            if (inSlice >= sliceSize) {
                inSlice = 0;
                parent->composer.platform->scheduler()->yield();
            }
        }
        handle->release(chunks);
    }
}

PtyEofReady::PtyEofReady(SessionSetImpl* parent_)
    : parent(parent_)
{
}

void PtyEofReady::ready() {
    parent->closeEndedSessions();
}

void ReapBody::run() {
    parent->runReaper();
}

SessionSetImpl::SessionSetImpl(Composer& composer_)
    : composer(composer_)
{
}

SessionSetImpl::~SessionSetImpl() noexcept {
    for (size_t at = 0; at < count_; ++at) {
        delete sessions[at].arena;
    }
    for (size_t at = 0; at < graveCount_; ++at) {
        delete graves[at].arena;
    }
    SessionSet::liveSessions = 0;
}

void SessionSetImpl::newSession() {
    ObjPool* const arena = ObjPool::fromMemoryRaw();
    PtyHandle* handle;
    Vterm* terminal;
    try {
        handle = composer.pty->spawn(*arena, *composer.launch);
        handle->resize(ptySize());
        terminal = Vterm::create(*arena, composer.geometry, composer.vtConfig, composer.extras, *composer.smallObjects, *composer.scheduler, *composer.host, *handle, composer.vtermTraceFactory);
    } catch (...) {
        delete arena;
        throw;
    }
    const Session session{terminal, handle, nextSessionId_++, arena, arena->make<Buffer>()};
    if (count_ < sessions.length()) {
        sessions.mut(count_) = session;
    } else {
        sessions.pushBack(session);
    }
    const size_t index = count_++;
    SessionSet::liveSessions = (sig_atomic_t)(count_);
    handle->engage();
    PtyReadBody* const reader = arena->make<PtyReadBody>(this, session.id, *handle, *terminal);
    // The parser is deep enough that this client fiber needs more than the
    // light leaf-fiber stack.
    composer.platform->scheduler()->create(*arena, *reader, 256 * 1024);
    activate(index);
    if (composer.window != nullptr) {
        composer.window->requestFrame();
    }
    if (composer.opts->vt.verbose) {
        fprintf(stderr, "%s: session: opened, %zu total\n", composer.brand->identifierCString(), count_);
    }
}

bool SessionSetImpl::close(size_t index) {
    if (index >= count_) {
        return count_ != 0;
    }
    // The terminal leaves the input chain before its slot is reused, or
    // the chain keeps a node pointing at a record that has moved.
    sessions[index].terminal->deactivate();
    const Grave grave{sessions[index].arena, sessions[index].terminal};
    for (size_t at = index; at + 1 < count_; ++at) {
        sessions.mut(at) = sessions[at + 1];
    }
    --count_;
    SessionSet::liveSessions = (sig_atomic_t)(count_);
    if (graveCount_ < graves.length()) {
        graves.mut(graveCount_) = grave;
    } else {
        graves.pushBack(grave);
    }
    ++graveCount_;
    if (count_ == 0) {
        // The window is closing with this last session; the renderer keeps
        // presenting the dead terminal until then, so its arena must not
        // drop. SessionSet retains it until its own teardown.
        publishSessionsChanged();
        return false;
    }
    if (index == active_) {
        // The neighbour that shifted into this slot, or the new last one
        // if the tail went.
        activate(index < count_ ? index : count_ - 1);
    } else {
        // A background tab went; whatever the user watches stays put,
        // only its index may have shifted.
        if (index < active_) {
            --active_;
        }
        publishSessionsChanged();
    }
    if (reaper_ != nullptr) {
        reaper_->wake();
    }
    return true;
}

void SessionSetImpl::activate(size_t index) {
    if (index >= count_) {
        return;
    }
    // Every terminal leaves the input chain first, including the incoming
    // one: Vterm::create puts each terminal on the chain as it is built,
    // so before the first activation more than one is on it.
    for (size_t at = 0; at < count_; ++at) {
        sessions[at].terminal->deactivate();
    }
    active_ = index;
    sessions[index].terminal->activate();
    // The window's state, replayed. A session that was not there when the
    // window gained focus or the pointer arrived still has to hear it.
    sessions[index].terminal->focus(focused_);
    sessions[index].terminal->pointerPresence(pointerPresent_);
    if (pointerKnown_) {
        sessions[index].terminal->pointerRepositioned(composer.layout.pointerPosition(pointerX_, pointerY_));
    }
    if (composer.opts->vt.verbose) {
        fprintf(stderr, "%s: session: activated %zu of %zu\n", composer.brand->identifierCString(), index + 1, count_);
    }
    // Every mutation of the tab model funnels through here (opening and
    // closing both end in an activation), so this is the one commit
    // point the chrome listens to.
    publishSessionsChanged();
}

bool SessionSetImpl::closeActive() {
    return close(active_);
}

CallSessionsLayoutChanged::CallSessionsLayoutChanged(SessionSetImpl* parent_)
    : parent(parent_)
{
}

void CallSessionsLayoutChanged::onListen(void*) {
    parent->layoutChanged();
}

void SessionSetImpl::layoutChanged() {
    if (count_ == 0) {
        return;
    }
    Vterm* const terminal = activeTerminal();
    if (pointerKnown_) {
        terminal->pointerRepositioned(composer.layout.pointerPosition(pointerX_, pointerY_));
    }
    terminal->expose();
}

VtPointerPosition SessionSetImpl::pointerPosition(int x, int y) {
    pointerKnown_ = true;
    pointerX_ = x;
    pointerY_ = y;
    return composer.layout.pointerPosition(x, y);
}

void SessionSetImpl::everyTerminalResized() {
    // Background sessions track the window too: a terminal that resized
    // only on activation would replay its scrollback into wrong geometry.
    for (size_t at = 0; at < count_; ++at) {
        sessions[at].terminal->windowResized();
        sessions[at].handle->resize(ptySize());
        debugTraceTerminal(composer, StringView(u8"resized"), *sessions[at].terminal);
    }
}

void SessionSetImpl::everyTerminalFontChanged() {
    for (size_t at = 0; at < count_; ++at) {
        sessions[at].terminal->presentationInvalidated();
        debugTraceTerminal(composer, StringView(u8"font-invalidated"), *sessions[at].terminal);
    }
}

void SessionSetImpl::everyTerminalConfigChanged() {
    for (size_t at = 0; at < count_; ++at) {
        try {
            sessions[at].terminal->configChanged();
        } catch (...) {
            // configChanged() allocates before it touches terminal state,
            // so a failure keeps this terminal on its previous
            // materialization without robbing the other sessions of the
            // reload.
        }
    }
}

void SessionSetImpl::titleChanged(const VtermTitleChanged& event) {
    // Every session's label follows its own terminal, for whatever chrome
    // projects the tab model.
    for (size_t at = 0; at < count_; ++at) {
        if (sessions[at].terminal != event.source || sessions[at].title == nullptr) {
            continue;
        }
        sessions[at].title->reset();
        sessions[at].title->append(event.title.data(), event.title.length());
        publishSessionsChanged();
        break;
    }
    // SessionSet owns visibility: a background OSC title is retained by
    // its terminal but cannot replace the active session's window chrome.
    if (count_ == 0 || event.source != activeTerminal()) {
        return;
    }
    publishWindowTitle(event.title);
}

void SessionSetImpl::publishWindowTitle(StringView title) {
    if (composer.window == nullptr) {
        return;
    }
    if (count_ < 2) {
        composer.window->requestTitle(title);
        return;
    }
    Buffer decorated;
    char prefix[32];
    const int length = snprintf(prefix, sizeof(prefix), "[%zu/%zu] ", active_ + 1, count_);
    if (length > 0) {
        decorated.append(prefix, (size_t)(length));
    }
    decorated.append(title.data(), title.length());
    composer.window->requestTitle(StringView(decorated));
}

void SessionSetImpl::runReaper() {
    plt::Fiber* const self = composer.platform->scheduler()->current();
    for (;;) {
        if (graveCount_ == 0 || count_ == 0) {
            // With no successor there can be no exposing frame that makes
            // the last terminal safe to reap. SessionSet teardown owns all
            // remaining graves.
            self->park();
            continue;
        }
        reapReady();
        if (graveCount_ != 0) {
            self->parkFor(gravePollUs);
        }
    }
}

void SessionSetImpl::reapReady() {
    size_t kept = 0;
    for (size_t at = 0; at < graveCount_; ++at) {
        const Grave grave = graves[at];
        if (canReap(grave.terminal)) {
            // Runs ~VtermImpl: the timer fibers are released off their
            // parked deadlines and the arena drops the whole terminal.
            delete grave.arena;
        } else {
            graves.mut(kept) = grave;
            ++kept;
        }
    }
    graveCount_ = kept;
}

bool SessionSetImpl::canReap(Vterm* terminal) const {
    (void)(terminal);
    // The renderer sheds the dead terminal's retained cells only when it
    // consumes the successor's full expose; until that frame lands, its
    // cell pointers still reach into the arena.
    if (composer.renderer != nullptr && activeTerminal()->presentationChanged()) {
        return false;
    }
    return true;
}

PtySize SessionSetImpl::ptySize() const {
    return {
        .columns = composer.geometry.columns,
        .rows = composer.geometry.rows,
        .pixelWidth = (u32)(composer.geometry.columns) * composer.geometry.cellPixelWidth,
        .pixelHeight = (u32)(composer.geometry.rows) * composer.geometry.cellPixelHeight,
    };
}

void SessionSetImpl::ptyEof(u64 sessionId) {
    endedSessions.pushBack(sessionId);
    eofWake_->signal();
}

void SessionSetImpl::closeEndedSessions() {
    for (size_t ended = 0; ended < endedSessions.length(); ++ended) {
        for (size_t at = 0; at < count_; ++at) {
            if (sessions[at].id != endedSessions[ended]) {
                continue;
            }
            const bool remains = close(at);
            if (composer.window != nullptr) {
                if (remains) {
                    composer.window->requestFrame();
                } else {
                    composer.window->requestClose();
                }
            }
            break;
        }
    }
    endedSessions.clear();
}

volatile sig_atomic_t SessionSet::liveSessions = 0;

SessionSet* SessionSet::create(Composer& composer) {
    SessionSetImpl* const sessions = composer.pool->make<SessionSetImpl>(composer);
    composer.sessions = sessions;
    // Behind InputBindings, which must keep first refusal on the chords.
    composer.inputHandlers.pushBack(sessions);
    composer.copyListeners.pushBack(&sessions->copyAction);
    composer.pasteListeners.pushBack(&sessions->pasteAction);
    composer.pastePrimaryListeners.pushBack(&sessions->pastePrimaryAction);
    composer.pageUpListeners.pushBack(&sessions->pageUpAction);
    composer.pageDownListeners.pushBack(&sessions->pageDownAction);
    composer.clearListeners.pushBack(&sessions->clearAction);
    composer.wordLeftListeners.pushBack(&sessions->wordLeftAction);
    composer.wordRightListeners.pushBack(&sessions->wordRightAction);
    composer.lineStartListeners.pushBack(&sessions->lineStartAction);
    composer.lineEndListeners.pushBack(&sessions->lineEndAction);
    composer.killLineListeners.pushBack(&sessions->killLineAction);
    composer.eraseWordListeners.pushBack(&sessions->eraseWordAction);
    composer.newTabListeners.pushBack(&sessions->newTabAction);
    composer.closeTabListeners.pushBack(&sessions->closeTabAction);
    composer.prevTabListeners.pushBack(&sessions->prevTabAction);
    composer.nextTabListeners.pushBack(&sessions->nextTabAction);
    for (unsigned at = 0; at < 9; ++at) {
        composer.selectTabListeners[at].pushBack(&sessions->selectTabActions[at]);
    }
    composer.resizedListeners.pushBack(&sessions->resizeAction);
    composer.layoutChangedListeners.pushBack(&sessions->layoutChangedAction);
    composer.fontChangedListeners.pushBack(&sessions->fontChangedAction);
    composer.configChangedListeners.pushBack(&sessions->configChangedAction);
    composer.titleChangedListeners.pushBack(&sessions->titleChangedAction);
    sessions->reaper_ = composer.platform->scheduler()->create(*composer.pool, sessions->reapBody);
    sessions->eofWake_ = composer.platform->createLoopWake(*composer.pool, sessions->eofReady);
    sessions->newSession();
    return sessions;
}

bool SessionSetImpl::activateNext() {
    if (count_ < 2) {
        return false;
    }
    activate((active_ + 1) % count_);
    return true;
}

bool SessionSetImpl::activatePrevious() {
    if (count_ < 2) {
        return false;
    }
    activate(active_ == 0 ? count_ - 1 : active_ - 1);
    return true;
}

bool SessionSetImpl::selectOrdinal(size_t ordinal) {
    if (count_ == 0) {
        return false;
    }
    // The ninth chord means "the last tab", iTerm style.
    const size_t index = ordinal == 8 ? count_ - 1 : ordinal;
    if (index >= count_ || index == active_) {
        return false;
    }
    activate(index);
    return true;
}

size_t SessionSetImpl::count() const {
    return count_;
}

size_t SessionSetImpl::activeIndex() const {
    return active_;
}

StringView SessionSetImpl::title(size_t index) const {
    if (index >= count_ || sessions[index].title == nullptr) {
        return {};
    }
    return StringView(*sessions[index].title);
}

void SessionSetImpl::publishSessionsChanged() {
    for (IntrusiveNode* node = composer.sessionsChangedListeners.mutFront(); node != composer.sessionsChangedListeners.mutEnd();) {
        Listener* const listener = static_cast<Listener*>(node);
        node = node->next;
        listener->onListen();
    }
}

Vterm* SessionSetImpl::activeTerminal() const {
    // There is no "no sessions" state to represent: the window opens its
    // first session before its loop starts and dies the moment the last
    // one closes. Closing never shrinks the storage, so even the twilight
    // frames between that close and the exit still have their terminal.
    return sessions[active_].terminal;
}

void CallSessionAction::onListen(void*) {
    Vterm* const terminal = parent->activeTerminal();
    switch (action) {
        case InputActions::Copy:
            terminal->copy();
            break;
        case InputActions::Paste:
            terminal->paste(false);
            break;
        case InputActions::PastePrimary:
            terminal->paste(true);
            break;
        case InputActions::PageUp:
            terminal->pageUp();
            break;
        case InputActions::PageDown:
            terminal->pageDown();
            break;
        case InputActions::Clear:
            terminal->clear();
            break;
        case InputActions::WordLeft:
            terminal->sendBytes(StringView(u8"\033b"), true);
            break;
        case InputActions::WordRight:
            terminal->sendBytes(StringView(u8"\033f"), true);
            break;
        case InputActions::LineStart:
            terminal->sendBytes(StringView(u8"\x01"), true);
            break;
        case InputActions::LineEnd:
            terminal->sendBytes(StringView(u8"\x05"), true);
            break;
        case InputActions::KillLine:
            terminal->sendBytes(StringView(u8"\x15"), true);
            break;
        case InputActions::EraseWord:
            terminal->sendBytes(StringView(u8"\x1b\x7f"), true);
            break;
        case InputActions::NewTab:
            try {
                parent->newSession();
            } catch (Exception& error) {
                sysE << StringView(u8"Warning: cannot open a new tab: ") << error.description() << endL;
            }
            break;
        case InputActions::CloseTab:
            if (parent->closeActive()) {
                parent->composer.window->requestFrame();
            } else {
                parent->composer.window->requestClose();
            }
            break;
        case InputActions::PrevTab:
            if (parent->activatePrevious()) {
                parent->composer.window->requestFrame();
            }
            break;
        case InputActions::NextTab:
            if (parent->activateNext()) {
                parent->composer.window->requestFrame();
            }
            break;
        case InputActions::SelectTab1:
        case InputActions::SelectTab2:
        case InputActions::SelectTab3:
        case InputActions::SelectTab4:
        case InputActions::SelectTab5:
        case InputActions::SelectTab6:
        case InputActions::SelectTab7:
        case InputActions::SelectTab8:
        case InputActions::SelectTab9:
            if (parent->selectOrdinal((size_t)(action) - (size_t)(InputActions::SelectTab1))) {
                parent->composer.window->requestFrame();
            }
            break;
        default:
            break;
    }
}

bool SessionSetImpl::key(const plt::KeyInput& input) {
    return activeTerminal()->key(input);
}

bool SessionSetImpl::text(const plt::TextInput& input) {
    return activeTerminal()->text(input);
}

bool SessionSetImpl::pointerMotion(const plt::PointerMotionInput& input) {
    return activeTerminal()->pointerMotion({pointerPosition(input.pixelX, input.pixelY), input.modifiers});
}

bool SessionSetImpl::pointerButton(const plt::PointerButtonInput& input) {
    return activeTerminal()->pointerButton({pointerPosition(input.pixelX, input.pixelY), input.button, input.pressed, input.modifiers, input.time});
}

bool SessionSetImpl::scroll(const plt::ScrollInput& input) {
    return activeTerminal()->scroll({pointerPosition(input.pixelX, input.pixelY), input.x, input.y, input.modifiers});
}

void SessionSetImpl::focus(bool focused) {
    focused_ = focused;
    activeTerminal()->focus(focused);
}

void SessionSetImpl::pointerPresence(bool present) {
    pointerPresent_ = present;
    activeTerminal()->pointerPresence(present);
}

void SessionSetImpl::flush() {
    activeTerminal()->flush();
}
