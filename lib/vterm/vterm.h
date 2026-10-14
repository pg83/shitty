/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/rect.h>
#include <lib/vterm/vt_geometry.h>
#include <lib/vterm/terminal_types.h>

#include <std/str/view.h>
#include <std/sys/types.h>

#include <stddef.h>
#include <stdint.h>
#include <plt/input.h>

namespace stl {
    class Input;
    class ObjPool;
    class SmallObjAllocator;
    class Output;
}

namespace plt {
    struct Scheduler;
}

struct VtCellExtras;
struct VtConfigSlot;
struct VtHost;
struct PtyHandle;
struct CellExtraStore;
struct Screen;
struct VtermTraceFactory;
struct Vterm;

struct VtermTitleChanged {
    Vterm* source;
    stl::StringView title;
};

enum class VtModifier : u8 {
    none = 0,
    shift = 1,
    control = 2,
    shift_control = 3,
    alt = 4,
    shift_alt = 5,
    control_alt = 6,
    shift_control_alt = 7,
    super = 8
};

constexpr VtModifier operator|(VtModifier lhs, VtModifier rhs) {
    return (VtModifier)((u8)(lhs) | (u8)(rhs));
}

constexpr VtModifier operator&(VtModifier lhs, VtModifier rhs) {
    return (VtModifier)((u8)(lhs) & (u8)(rhs));
}

enum class MouseTrackingMode : u8 {
    Disabled = 0,
    X10_Compat,
    VT200,
    VT200_ButtonEvent,
    VT200_AnyEvent,
    VT200_Highlight
};
enum class MouseTrackingEnc : u8 {
    Default = 0,
    UTF8,
    SGR,
    URXVT,
    SGRPixels
};

struct MouseTrackingState {
    MouseTrackingMode mode = MouseTrackingMode::Disabled;
    MouseTrackingEnc enc = MouseTrackingEnc::Default;
    bool focusEventMode = false;
    u32 generation = 0;

    void setMode(MouseTrackingMode value);
    void setEncoding(MouseTrackingEnc value);
};

struct RectangleOrigin {
    u16 rowBase;
    u16 columnBase;
    u16 rowLimit;
    u16 columnLimit;
};

enum class VtermKeyEventType : u8 {
    Press = 1,
    Repeat = 2,
    Release = 3
};

struct VtermTextResult {
    stl::StringView text;
    bool status = false;
};

// A mode snapshot for embedders that surface terminal state without
// reaching into the protocol - what an emulator wrapper reports as
// "modes". Presentation state travels with TerminalUpdate instead.
struct VtermState {
    MouseTrackingMode mouseTracking = MouseTrackingMode::Disabled;
    MouseTrackingEnc mouseEncoding = MouseTrackingEnc::Default;
    bool synchronizedOutput = false;
    bool alternateScreen = false;
    bool bracketedPaste = false;
    bool applicationCursorKeys = false;
    bool applicationKeypad = false;
    bool focusEvents = false;
    bool autoWrap = false;
    bool originMode = false;
    bool insertMode = false;
    bool showCursor = false;
    bool screenReverse = false;
    // DECSET 1007: on the alternate screen the wheel sends arrow keys
    // rather than moving a history the alternate screen does not have.
    bool alternateScroll = false;
};

struct TerminalUpdate {
    // The damaged view rows, ascending; each re-renders wholly.
    const TerminalRow* rows = nullptr;
    size_t rowCount = 0;
    // A9: the grid of the pane this frame belongs to, which is the grid
    // its cells were allocated and indexed by - not the window's. Zero
    // is a refused frame, not a window-sized default: a renderer that
    // reads zero here returns false, the way it already refuses a null
    // colors. The default is absent on purpose, because a field everyone
    // filled in with the window would hide the very gap it marks.
    u16 gridColumns = 0; // width of TerminalRow::cells and its indexing stride
    u16 gridRows = 0;    // height of the grid row.row indexes into
    // The model behind this frame: a strip-consuming renderer reads its
    // view rows and shapes them through the embedder's span shaper. Null
    // when the update is synthesized without a screen (renderer-internal
    // repaints).
    Screen* shapes = nullptr;
    // The preedit preview: overlayCount cells drawn over overlayRow from
    // overlayColumn, covering the row content beneath them. They exist
    // outside the screen model, so the renderer shapes them itself as a
    // loose cell run. Zero count when no preview is active.
    const TerminalCell* overlayCells = nullptr;
    u16 overlayRow = 0;
    u16 overlayColumn = 0;
    u16 overlayCount = 0;
    // Every row carries cells foreign to the model (retained cells
    // re-rendered under a rebuilt presentation); the renderer shapes
    // each row as a loose cell run instead of the screen rows.
    bool shapeFromCells = false;
    const TerminalColors* colors = nullptr;
    u32 viewOffset = 0;
    u32 historyRows = 0;
    TerminalCursor cursor;
    Rect selection;
    Rect snappedSelection;
    Color selectionForeground;
    Color selectionBackground;
    u8 selectionColorMask = 0;
    u32 hoveredHyperlink = 0;
    u32 hoveredLinkBegin = 0;
    u32 hoveredLinkEnd = 0;
    bool screenReverse = false;
    bool blinkVisible = true;
    bool cursorBlink = false;
};

struct Vterm {
    // A5: "visible" and "focused" are two states, not one. This is
    // visibility - the terminal is a pane of the tab on screen, so it
    // renders and wakes frames. It says nothing about input: a tab shows
    // many panes and exactly one of them is focused, and inventing focus
    // here would flicker a lie at a child watching for the events.
    //
    // Makes the terminal's presentation current and repaints it. The
    // repaint is not optional: a renderer may retain cells from the
    // presentation it consumed before this one.
    virtual void show() = 0;
    // A5: off screen - the tab went to the background, or the pane was
    // closed. Ends the terminal's current presentation and drops its
    // input focus with it, since a terminal nobody can see cannot be the
    // one taking input. The converse does not hold: losing the focus to
    // a neighbouring pane leaves this one visible.
    virtual void hide() = 0;
    // Input callbacks are invoked by whichever client currently presents
    // this terminal; Vterm does not join a global input router itself.
    virtual bool key(const plt::KeyInput& input) = 0;
    virtual bool text(const plt::TextInput& input) = 0;
    virtual bool pointerMotion(const plt::PointerMotionInput& input) = 0;
    virtual bool pointerButton(const plt::PointerButtonInput& input) = 0;
    virtual bool scroll(const plt::ScrollInput& input) = 0;
    virtual void focus(bool focused) = 0;
    virtual void pointerPresence(bool present) = 0;
    virtual void flush() = 0;
    // Terminal actions are likewise invoked by the presenting client.
    virtual void copy() = 0;
    virtual void paste(bool primary) = 0;
    virtual void pageUp() = 0;
    virtual void pageDown() = 0;
    // Moves the view through the scrollback: positive rows scroll up
    // into history, negative back toward the live bottom. Both clamp to
    // the retained history and return the resulting offset, which is 0
    // once the view is live again.
    virtual u32 scrollView(i32 rows) = 0;
    virtual u32 scrollViewTo(u32 offset) = 0;
    // What Ctrl+L means, reached from a platform's own chord. The byte
    // goes to the shell rather than clearing here, so the shell's own
    // idea of a clear - prompt redraw and all - is what happens.
    virtual void clear() = 0;
    virtual void feedPty(stl::StringView bytes) = 0;
    // One batch, one round of cursor and presentation bookkeeping: the
    // pty drain hands over whole blocks, and paying the per-feed wrap
    // per block would cost more than the parse.
    virtual void feedPty(const stl::StringView* slices, size_t count) = 0;
    virtual void expose() = 0;
    // expose() only marks the output pending; exposeAll() damages the
    // rows of both screens, so that the next frame hands over the pane
    // whole. It is what a window answers a refused frame with: a
    // renderer refuses a frame it finds incomplete, and the same frame
    // asked for again is incomplete in the same way (application.cpp).
    virtual void exposeAll() = 0;
    virtual void sendBytes(stl::StringView bytes, bool userInput) = 0;
    // The command-line editor at a zsh prompt (prompt_editor.h). Selects
    // the command line, or undoes or redoes an edit of it through zsh's own
    // undo; false, with nothing done, when there is no line to edit.
    virtual bool selectCommandLine() = 0;
    // Whether there is a command line to edit now: the chords that act on
    // one are the program's whenever there is not.
    virtual bool commandLineEditable() const = 0;
    virtual bool commandLineUndo(bool redo) = 0;
    // Input-method composition preview, rendered as an overlay on the
    // cursor row of the emitted frame; never enters the screen model,
    // the scrollback, or the pty. Empty text clears the preview.
    // cursorBegin/cursorEnd are byte offsets into text, or -1 when the
    // input method hides its cursor. The preview clusters like printed
    // text, so it shows what the grid will hold once the composition
    // commits.
    virtual void preedit(stl::StringView text, i32 cursorBegin, i32 cursorEnd) = 0;
    // Text dropped onto the window by a drag-and-drop session; the stream
    // is pulled on the calling fiber chunk by chunk under the PTY mutex,
    // with the same sanitizing and bracketed-paste treatment as a
    // clipboard paste. Off fibers the payload is buffered whole (bounded)
    // and replayed from a transaction.
    virtual void dropText(stl::Input& source) = 0;
    // A text/uri-list drop: every entry is inserted shell-quoted with a
    // trailing separator through the same paste path as dropText().
    virtual void dropUriList(stl::Input& source) = 0;

    // Reads the pty's foreground process and, when its name differs
    // from the last one seen, makes that name the title - displacing
    // whatever the previous foreground left behind. While the name is
    // stable an application's own title stands untouched. Driven for
    // the active terminal by the embedder's one polling timer.
    virtual void refreshForegroundName() = 0;

    virtual bool expireSynchronizedOutput(bool force) = 0;
    virtual bool advanceAnimation(bool force) = 0;
    virtual const TerminalUpdate* output() = 0;
    // A2/R7-2: this pane's current presentation with nothing damaged -
    // the update output() would have returned, save that rowCount is
    // zero, so a renderer keeps the cells it already retains for this
    // pane and draws them where this frame puts it.
    //
    // Exists because a frame is a list of panes and every live pane owes
    // it an entry: a pane with nothing to say (no output pending, or
    // holding a synchronized-output batch) would otherwise drop out of
    // the list, the pane count would change, and both backends would
    // refuse the frame as a reshape until every pane damaged itself
    // whole - the full repaint A3 exists to avoid, on every frame a
    // neighbour happens to be quiet.
    //
    // Never null, and never a substitute for output(): it neither
    // captures damage nor arms consume(), so a pane that does have a
    // frame must still be asked through output() or its damage stays
    // pending. The grid is this pane's own, and a terminal without one
    // hands back zero columns - a refused frame, not a window-sized
    // default (A9, see TerminalUpdate::gridColumns). The reference is
    // this terminal's own storage, valid until the next call on it.
    virtual const TerminalUpdate& retainedOutput() = 0;
    virtual void consume() = 0;
    virtual VtermState state() const = 0;

    // A8: this pane's geometry changed: adopt it and redraw. Delivered to
    // every session, background ones included - a terminal that resized
    // only on activation would come back wrong. Upstream's windowResized()
    // is what this replaced: the window's grid is not this pane's.
    //
    // One call, not a setter plus a trigger: the grid rebuild reflows the
    // scrollback and reports CSI 48 to the child, so a second idle pass
    // over it would send the shell a phantom resize report it never asked
    // for. There is no way to run it twice by accident when running it is
    // the only thing this does.
    virtual void paneResized(const VtGeometry& geometry) = 0;
    // A configuration snapshot replaced the one behind VtConfigSlot::config;
    // the terminal re-materializes what it derived. Allocates before it
    // touches state: a throw leaves the previous materialization intact,
    // and the owner delivering the reload must keep walking its other
    // terminals.
    virtual void configChanged() = 0;
    // The embedder rebuilt how cells become pixels - every retained row
    // it holds is stale. Re-expose the whole screen and redraw.
    virtual void presentationInvalidated() = 0;
    // Whether the presentation moved past what the renderer last
    // consumed.
    virtual bool presentationChanged() const = 0;
    // A11: the cells this terminal holds - its primary screen and, once
    // it has one, its alternate. A store shared by the whole window is
    // sized by the sum of this over every live pane, which is the only
    // number that stays right when the panes are of different sizes.
    virtual size_t cellCapacity() const = 0;

    // The terminal and everything it owns - fiber stacks, screens - come
    // out of owner, which is what lets a session die by dropping its
    // arena.
    //
    // A8: the geometry is a parameter rather than something read off the
    // embedder, because the very first screen is already sized to it. A
    // terminal that read the window at birth and only accepted a pane
    // afterwards would allocate the window's grid, reflow it once, and
    // hand its child a resize it never asked for.
    //
    // Upstream's explicit pieces, in upstream's order, plus one of ours.
    // Both geometries are VtGeometry since T5.1, and they are two
    // instances rather than one: windowGeometry is the window's surface
    // and the cell size the font gives it, shared by every pane on it,
    // while `geometry` is this pane's own rectangle, border and grid.
    // The embedder writes the first through VtHost::surfaceResized() and
    // the second through paneResized(), and neither ever writes the
    // other.
    //
    // Ten parameters against upstream's nine, and the difference is the
    // pane geometry alone. The Composer& that stood second until now is
    // gone: the three questions the core still had for the embedder -
    // the window's content insets, the in-band resize and the pane
    // list's cell count - are VtHost methods, so the core names an
    // interface it owns rather than a struct that lives in lib/shitty.
    static Vterm* create(stl::ObjPool& owner, VtGeometry& windowGeometry, const VtConfigSlot& config, VtCellExtras& extras, stl::SmallObjAllocator& smallObjects, plt::Scheduler& scheduler, VtHost& host, const VtGeometry& geometry, PtyHandle& pty, VtermTraceFactory* traceFactory);
};
