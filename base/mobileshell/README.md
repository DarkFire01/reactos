# ReactOS Mobile Shell (Windows Mobile 6.1 skin)

A phone shell with the Windows 10 Mobile architecture, wearing a Windows Mobile 6.1 skin.
The shell logic knows nothing about how things look: every pixel comes from markup (`.uix`)
and a theme, so a W10M skin is a new folder under `skins\`, not new code.

## Build and run

```
build.cmd
build\bin\PhoneShellUI.exe [--scale 2] [--theme Blue] [--run "script"]
```

`--run` takes `;` separated steps, one every 400 ms: any shell command, `tap:x,y`,
`press:x,y` / `release:x,y` (split taps, so tap and hold can run), `wheel:x,y,notches`,
`type:text`, `key:F1`, `shot:file.png`, `wait`. Coordinates are logical. The window stays
topmost while scripted because shots read the screen.

Keys (remap in `shell.xml`): F1/F2 soft keys, F3 input panel, LWin Start, Home Today,
Esc back, arrows and Enter navigate, F5 reload markup, F9 next theme. Typing goes to the
active app.

Apps: Settings, Contacts, Calendar, Calculator, Programs, Getting Started and ReactOS Office
Mobile (Writer, Sheets, Slides and Notes in `office.exe`; the Office folder, Programs and Getting
Started are markup only, hosted by `markupapp.exe`). Data lives in `%LOCALAPPDATA%\ReactOSMobile`
(Office documents in `My Documents`), the owner card in `HKCU\Software\ReactOSMobile\Owner`.
Launching anything not registered posts a notification.

File Explorer (`files.exe`) browses My Device (My Documents plus every volume) with the 6.1
folder and sort drop downs, tap and hold for Cut, Copy, Rename, Delete and Properties, image
previews, and hands `.txt`, `.rtf`, `.csv` and `.slides` to the Office programs (`Launch` with a
document, `--open` at start or `OpenDocument` when already running). It changes files inside My
Documents only, unless its `App` entry passes the writable switch set to `all`.

The title bar is live (`include\devstatus.h`): battery level and charging, network with Wi-Fi
signal bars, and the system volume. Tapping it opens the status panel, which sets the volume.
Settings has real pages for owner info, sounds, theme, power, clock, memory, regional, screen,
network, running programs, installed programs and the key map. `Args="--page Power"` on an
`App` in `shell.xml` makes a shortcut straight to one page.

## Architecture

```
PhoneShellUI.exe   host: window, boot order, co-render frame loop, input, key remap, skins
  CoreMessaging.dll  message session: endpoints, named registrar, queued Send, SendSync
  UIXRender.dll      UIX runtime: markup, theme, layout, D2D drawing, DComp layers
  ShellChrome.dll    AppChromeServerEndpoint: title bar, status, soft keys
  MobileUI.dll       ShellHostEndpoint: Today, Start menu, out of process app host
*.exe apps           AppClient + the same UIX runtime, offscreen, one process each
```

MobileUI also owns the shell services apps reach over the pipe: soft key and context menus
(`popup.uix`), the notification bubble and queue (`notify.uix`), the soft input panel
(`sip.uix`) and the tap and hold ring (`hold.uix`). The host times tap and hold (dots at
150 ms, the hold fires at 650 ms) and cancels it past a 6 px move.

Same module split and contracts as the W10M shell (PhoneShellUI, ShellChrome, MobileUI,
UIXRender, CoreMessaging). The host stands where DWM stood and drives the engine through
`PreRender / BeginRender / Render / EndRender / PostPresent`.

### Composition

* Every element with `Layer="true"` gets its own DComp visual and surface. Other elements draw
  into the nearest layer. Only dirty layers are redrawn; moving or hiding a layer is a
  compositor operation (offset or empty clip), not a redraw.
* Layers are drawn with D2D into an offscreen texture and copied into the DComp surface. That
  path needs only `BeginDraw` with `IDXGISurface`, which the ReactOS dcomp already supports.
* `Reveal="dx,dy,seconds"` on a layer runs a compositor side ease out when it is shown.
* The host owns three z ordered bands: content (home, then apps), chrome, overlay (Start menu).

### Apps (sandbox ready)

* Each app is its own process in a Job object (kill on close, UI restrictions). The shell talks
  to it over one message mode pipe (`include\appwire.h`) and checks the client PID.
* The app renders into a shared D3D11 texture (keyed mutex when available) and the shell
  copies it into a DComp surface. Input, soft keys, chrome and lifecycle go over the pipe.
  Nothing else crosses the boundary, so the Job can later become a container or a VM.
* `Scale="Native"` apps render at the shell's pixel density. `Scale="Legacy"` apps render at
  the base QVGA resolution and the compositor scales them.
* Close button: `X` hides the app and leaves it running, `ok` is passed to the app to decide.

## Skins and themes

`shell.xml` picks the skin; `skins\<name>\skin.xml` sets the logical canvas and lists themes.
Themes are XML with `Color`, `Brush` (solid, gradient, image), `Font`, `Metric`, `Image`,
`ImageSet` and `String`, and can inherit with `Base=`. Markup refers to them with `@Name`,
and to runtime data with `{Binding}`. A binding can hold a theme reference, so state can
switch brushes (`Background="{TabPersonal}"`).

### Skin contract

A skin provides these files. Anything not listed is free form.

| File | Required names |
|---|---|
| `chrome.uix` | bindings `Title`, `SoftKeyLeft`, `SoftKeyRight`, `CloseLabel`, `Time`, `Battery`; elements `CloseButton`, `Clock`; commands `start`, `softkey.left/right/center`, `close` |
| `home.uix` | root attributes `Title`, `SoftKeyLeft(Command)`, `SoftKeyRight(Command)`, `AppReveal`; element `AppHost`; bindings `Weekday`, `Date`, `Time`, `UserName` |
| `startmenu.uix` | element `StartMenuRoot` |
| `popup.uix` (optional) | elements `PopupRoot`, `PopupMenu`; repeater `PopupItems` (fields `Text`, `Height`, `Fill`, `Command`); bindings `PopupX`, `PopupY` |
| `notify.uix` (optional) | root `SoftKeyLeft`, `SoftKeyRight`; element `NotifyRoot`; bindings `NotifyTitle`, `NotifyBody` |
| `sip.uix` (optional) | elements `SipRoot`, `SipLetters`, `SipSymbols`; bindings `K.<char>`, `ShiftFill`, `CapsFill`, `SymbolsFill` |
| `hold.uix` (optional) | element `HoldRoot`; bindings `HoldX`, `HoldY` |
| `status.uix` (optional) | element `StatusRoot`; bindings `StatusNetwork`, `StatusVolume`, `StatusBattery`, `MuteLabel`, `VolumeGlyph`, `Vol1..Vol10` |
| `chrome.uix` status | root `BatteryFillWidth`; bindings `BatteryLevel`, `BatteryBrush`, `Bar1..Bar5`, `VolumeGlyph`; elements `BatteryCharging`, `NetActive`, `NetOff`; command `status` |
| `chrome.uix` extra | element `NotifyIcon` (shown while notifications are pending) |
| theme strings | `CloseOk`, `CloseX`, `NotInstalled`, `NotifyDismiss`, `NotifyHide` |

Shell commands usable from any markup: `start`, `start.dismiss`, `home`, `launch:<AppId>`,
`back`, `nav.up/down/select`, `sip`, `sip.<key>`, `notify:<Title>|<Body>`, `notify.show`,
`notify.hide`, `notify.dismiss`, `popup.dismiss`, `status`, `volume.up/down/mute`, `theme:<Name>`, `theme.next`, `reload`, `exit`.

### Markup reference

Elements: `Element`, `Text`, `Image`, `Repeater`. Layouts: `Overlay` (default), `Stack`,
`Row`, `Wrap`. Sizes: number, `auto`, `*` or `n*`. Common attributes: `Name`, `Width`,
`Height`, `X`, `Y`, `Margin`, `Padding`, `HAlign`, `VAlign`, `Background`, `BorderBrush`,
`BorderThickness`, `CornerRadius`, `Visible`, `Layer`, `Command`, `HoldCommand`,
`Selectable`, `Momentary`, `SelectedBackground`, `SelectedForeground`. Inherited:
`Foreground`, `Font`, `TextAlign`. Text: `Content`, `Wrap`, `TextVAlign`, `Collapse`, `Highlight` (bolds and underlines matches).
Image: `Source`, `Stretch`.

* `Scroll="true"`: a vertical viewport. Drag past `@DragSlop` scrolls instead of tapping,
  the wheel scrolls `ScrollStep`, `ScrollThumbBrush` / `ScrollBarBrush` / `ScrollBarWidth`
  draw the bar. Children outside the viewport are culled. There is no kinetic scrolling,
  matching 6.1 (flick physics arrived in 6.5).
* `Repeater`: its first child is a template, filled from code with `SetItems`; the template
  reads `{Item.Field}` and `{Item.Index}`, and `Name="c{Item.Id}"` makes items addressable.
* Layer effects, all run by the compositor: `Reveal="dx,dy,seconds[,opacity]"` eases in on
  show, `Opacity` sets a constant, `Spin="seconds"` rotates forever.

### App wire messages

Shell to app: Activate, Pointer (Down/Move/Up/Cancel/Hold), Key, Char, Wheel, SoftKey,
CloseButton, MenuPick, Theme, Terminate. App to shell: Ready, Presented, Chrome, Menu
(anchored to a soft key or a point), Notify, InputPanel, Launch (registered ids only),
RequestHide, RequestClose, Request (theme, status refresh and home only). All fixed size, see `include\appwire.h`.

## Not done yet

Phone, Messaging, Internet, File Explorer and the other Programs entries (they notify that
they are not installed), contact categories and the Calendar Day/Week/Year views, word
completion above the SIP, international (áü) and Ctl keys, notification timeouts.
