Да — на текущем master `b69e2730a53e` лаг вполне объясним кодом. `alt screen` не является первопричиной. Проблема начинается, когда scroll перестаёт быть локальным и превращается в поток mouse-report’ов для Claude Code.

```text
NSEvent
  → несколько wheel steps
  → N SGR mouse reports
  → N fiber-транзакций
  → N PTY-блоков / минимум N write(2)
  → Claude обрабатывает N scroll-команд
  → перерисовка через synchronized output
```

### Что происходит в Shitty

macOS precise delta без привязки к размеру ячейки просто умножается на `0.1`: [platform_cocoa.mm](/home/pg/monorepo/shitty/ext/plt/platform_cocoa.mm:1890). `phase`, `momentum`, `precise` и `time` передаются дальше, но фактически scroll-аккумулятором не используются.

Полученное целое число шагов разворачивается в цикл: [vterm.cpp](/home/pg/monorepo/shitty/vterm.cpp:2144). Каждый шаг отдельно вызывает `writePty`: [vterm.cpp](/home/pg/monorepo/shitty/vterm.cpp:2026).

Дальше каждый report:

- создаёт отдельную fiber-транзакцию: [vterm.cpp](/home/pg/monorepo/shitty/vterm.cpp:1424);
- копируется в отдельный PTY block и немедленно flush’ится: [vterm.cpp](/home/pg/monorepo/shitty/vterm.cpp:9226);
- drain thread пишет каждый block отдельным `write(2)`: [pty.cpp](/home/pg/monorepo/shitty/pty.cpp:704).

То есть один энергичный trackpad event способен породить несколько полностью независимых операций от Cocoa до PTY.

### Что делают конкуренты

| Реализация | Один native event | Доставка в PTY |
|---|---:|---|
| [WezTerm](/home/pg/monorepo/tmp/terminal-repos/wezterm/term/src/terminalstate/mouse.rs:76) | 1 report | величина wheel event фактически не размножается |
| [xterm.js](/home/pg/monorepo/tmp/terminal-repos/xterm.js/src/browser/services/MouseService.ts:104) | 1 report | один data event |
| [Windows Terminal](/home/pg/monorepo/tmp/terminal-repos/windows-terminal/src/cascadia/TerminalControl/ControlInteractivity.cpp:526) | 1 report | одна input operation |
| [iTerm2](/home/pg/monorepo/tmp/terminal-repos/iterm2/sources/PTYSession/PTYSession.m:13154) | N, максимум 32 | все reports склеиваются в один `NSData`, затем общий write buffer |
| [Kitty](/home/pg/monorepo/tmp/terminal-repos/kitty/kitty/mouse.c:1538) | N | reports добавляются в общий contiguous buffer, который пишется целиком: [child-monitor.c](/home/pg/monorepo/tmp/terminal-repos/kitty/kitty/child-monitor.c:1609) |
| [Alacritty](/home/pg/monorepo/tmp/terminal-repos/alacritty/alacritty/src/input/mod.rs:725) | N | отдельные queue items |
| [Ghostty](/home/pg/monorepo/tmp/terminal-repos/ghostty/src/Surface.zig:3422) | N | отдельные small-write messages, но IO thread дренирует пачку и объединяет redraw |
| [Rio](/home/pg/monorepo/tmp/riotest/frontends/rioterm/src/screen/mod.rs:3364) | N | отдельные `Msg::Input` и write-list entries |
| Contour, Foot, Konsole | N | преимущественно отдельная запись на report |
| GNOME Terminal/VTE | N | общий outgoing buffer, но возможна запись после каждого report |
| xterm | один дискретный X11 Button4/5 event | общий маленький output buffer |

Особенно показательно, что у Alacritty и Rio альтернативный scroll через стрелки уже собирается в один буфер, а mouse-reporting path — нет. У Shitty такой асимметрии тоже стоит избежать.

### Итоговый диагноз

Моя оценка по вероятности:

1. **Главный кандидат:** размножение wheel event в N reports плюс N fiber/PTY transactions. Это отличает Shitty от WezTerm/xterm.js/Windows Terminal и от батчинга Kitty/iTerm2.
2. **Второй кандидат:** фиксированные `10 px/step`, сохранение дробного остатка между жестами и игнорирование phase/momentum. У WezTerm есть reset по смене направления и после паузы; Kitty/Ghostty/Alacritty нормализуют по высоте ячейки.
3. **Не главный кандидат:** alternate screen или его grid/rendering. В headless-проверке текущего кода полный repaint alternate buffer был не медленнее primary. Кроме того, master уже держит display link тёплым около 60 кадров: [platform_cocoa.mm](/home/pg/monorepo/shitty/ext/plt/platform_cocoa.mm:1183).
4. Synchronized output поддержан и блокирует промежуточные кадры: [vterm.cpp](/home/pg/monorepo/shitty/vterm.cpp:3192). Он может сделать очередь визуально ступенчатой, но сам по себе не объясняет, почему очередь возникает.

Самый чистый A/B: сначала собрать все SGR reports одного `ScrollInput` в один `writePty`, не меняя число логических шагов. Если лаг останется — временно ограничить mouse tracking одним report на native event, как WezTerm/xterm.js. Эти два эксперимента однозначно разделят PTY/fiber overhead и backlog уже внутри Claude Code.

Код не менял.

### Обновление после реализации

Все три независимых направления реализованы:

- `VtermImpl::flush()` больше не запрашивает кадр без `outputPending`, поэтому входной event не вызывает repaint неизменённого экрана. Коалесцирование в plt по-прежнему работает, но больше не используется для подавления заведомо лишнего repaint.
- Все mouse reports одного `ScrollInput` собираются в один буфер и уходят одной PTY-транзакцией и одним flush. Число и порядок отчётов не изменились.
- `VtScroll` теперь сохраняет `phase`, `precise`, `momentum`, `time` и `contentScale`. Плавные delta нормализуются по фактическим размерам ячейки, а аккумулятор сбрасывает остаток на границах жеста, при смене направления, смене momentum-режима и после паузы более 100 мс.

Unit tests и полная native black-box suite проходят. Практический эффект на задержку в интерактивных TUI ещё нужно измерить на macOS; тесты подтверждают один write на native event, отсутствие кадра без изменений и новые правила квантизации.
