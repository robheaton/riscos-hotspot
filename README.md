# Hotspot — a RISC OS desktop controller for WPSD hotspots

A native RISC OS 5 Wimp application for watching and controlling a digital
voice hotspot (MMDVM) running **WPSD** (the Pi-Star successor) from the
RISC OS desktop. Nothing is installed on the hotspot: the program uses the
hotspot's own web dashboard, over plain HTTP on the LAN, exactly as a browser
does.

![icon](docs/icon.png)

## What it does

An icon bar icon opens one window with five tabs:

| Tab | What you see / do |
| --- | --- |
| **Status** | Radio state (IDLE / RX / TX / OFFLINE), frequency and modem, every mode and network the dashboard reports, with a **Pause / Resume** button on each mode (the dashboard's *Instant Mode Manager*); hardware cards (CPU, temperature, RAM, uptime…) |
| **Heard** | The last heard list (UTC time, callsign, mode, target, duration/loss); a live transmission is highlighted |
| **DMR** | For a DMRGateway hotspot (BrandMeister + TGIF side by side): the DMR networks with an **Enable / Disable** switch each (the dashboard's *DMR Network Manager*); BrandMeister's static talkgroups (**Link / Drop / Delete**), **Add TG…**, dynamic talkgroups with their timeouts, **Drop QSO**, **Drop dynamic**; TGIF's linked talkgroup per timeslot with **Link… / Unlink** |
| **Links** | **Link… / Unlink** for YSF, D-Star, P25 and NXDN, showing what each is linked to now; the last three targets used are one-click buttons |
| **System** | Restart the WPSD services, update the host files, **Reboot**, **Shut down** (all confirm first); save a diagnostics report |

The Menu button gives the same actions as menus (over the window, and over
the icon bar icon); F5 refreshes (click in the window first, so it has the
keyboard). The icon bar menu starts with **Info**, which, as in every RISC OS
program, is a submenu: moving the pointer onto its arrow opens the *Program
information* window (name, purpose, author, version and build date, laid out
like the ROM applications'), and it goes away with the menu. Settings (address, port, Admin login,
refresh interval, rows of last heard, BrandMeister on/off, operator names
on/off) are in *Choices…* and are kept in `Choices:Hotspot.Choices`
(the password in plain text, as is usual on RISC OS).

Polling only happens while the window is open, and follows the visible tab:
the radio state every refresh interval (default 5 s) and the mode/network
status every third cycle always; the last heard list every cycle on the
Heard tab; BrandMeister and TGIF every third cycle and the (large) DMR
network page every sixth on the DMR tab; the hardware cards every sixth on
Status and System. Switching tab fetches that tab's data at once.

### When the hotspot's address changes

The address is a setting (*Choices…*; the copy of `Choices` shipped in the
application has the usual one, and the first run shows it for checking and
for the password to be entered). It can be found again without typing:
**Find hotspot…** (in the window next to *Refresh*, beside the "Not
connected" message, and in both menus) tries every address 1–254 in a /24 —
it starts from the neighbourhood of the address in use — with the same
non-blocking client, twelve at a time, asking each for the radio status page;
anything that answers like a WPSD dashboard is offered ("Found a hotspot at
10.0.0.31 (MMDVM_HS_Hat-v1.6.1). Use this address?"). A fixed address for the
hotspot in the router (a DHCP reservation) avoids it altogether.

## How it talks to WPSD (and why nothing goes on the hotspot)

WPSD has no single remote-control API, but it has three surfaces that are
enough. All of this was read from the dashboard's source
(`https://repo.w0chp.net/WPSD-Dev/WPSD-WebCode`, scripts in `WPSD-Scripts`),
not guessed:

* **Public, no login** (the dashboard's own AJAX fragments):
  `GET /api/?limit=N&names=false&country=false` (JSON last heard),
  `/mmdvmhost/radioinfo.php` (radio state table),
  `/mmdvmhost/repeaterinfo.php` (the mode/network "status pills"),
  `/includes/hw_info.php` (CPU/RAM/temperature cards),
  `/mmdvmhost/tgif_links.php` (the TGIF talkgroup on each timeslot).
* **WPSD's own remote API**, HTTP Basic auth (user `pi-star` and the Admin
  password): `/admin/system_api.php?action=…&format=json` — `reboot`,
  `shutdown`, `restart_wpsd_services`, `update_hostfiles`,
  `action=bm_manager&cmd=link_static|drop_static|drop_dynamic|drop_qso` and
  `action=dmrnet_set_status&dmrNet=net4&netState=enable|disable`.
* **The dashboard's own forms**, POSTed exactly as the web page does:
  `/admin/index.php?func=mode_man` (Pause/Resume a mode),
  `func=ysf_man|p25_man|nxdn_man|ds_man|tgif_man` (reflector links),
  `/admin/bm-manager.php` (add/delete a static TG; its page is also read to
  list the TGs), `/admin/index.php?func=dmr_man` (read for the DMR network
  switches).

The HTML fragments are parsed *generically* (sections of titled "pills";
unknown sections and extra pills are shown, not rejected), so a dashboard
update that adds a row does not break the client. Mode buttons are attached
by pill position/label rather than by the (translatable) section title, so a
dashboard set to another language still works.

MMDVMHost's own remote control is deliberately not used: the WPSD
`mmdvmhost` config has no `[Remote Control]`/`[MQTT]` section by default,
and current upstream MMDVMHost has moved that interface to MQTT.

## Layout

```
src/            portable core (no RISC OS dependencies; also built on Linux)
  util.c        strings, growable buffer, base64, url-encoding, UTF-8 -> Latin-1
  http.c        non-blocking HTTP/1.1 client state machine (Basic auth,
                Content-Length / chunked / read-to-close bodies)
  json.c        small JSON reader        html.c   just-enough HTML reader
  wpsd.c        WPSD URLs + form fields (request builders) and parsers
  scan.c        the search of a /24 for a hotspot (Find hotspot…)
  client.c      job queue, polling schedule, the parsed "model", diagnostics
  rows.c        model -> display rows (what the window draws)
src/ro_*.c      the RISC OS front end (OSLib, windows built in code)
  ro_main.c     start-up, icon bar, poll loop, message/error boxes
  ro_info.c     the Program information window (the icon bar menu's Info submenu)
  ro_win.c      the main window: custom redraw + hit-testing of rows/buttons
  ro_dlg.c      one reusable dialogue window (Choices, link prompts…)
  ro_menu.c     icon bar menu and window menu    ro_act.c  what each action does
  ro_choices.c  Choices file and diagnostics report
test/           host tests, mock hotspot, fake desktop (see below)
tools/mksprites.py   draws the !Sprites file (32bpp, 34x34, no mask)
tools/mkdist.py      zips the build like RISC OS does (file types in "ARC0" fields)
tools/deliver-nas.sh copies the build to the NAS and verifies it byte for byte
app/!Hotspot/   hand-written application files (!Run, !Boot, !Help, Choices)
app/ReadMe,fff  the first-test instructions that go beside the application
```

## Building

The RISC OS build uses the same recipe as the other native projects here:
GCCSDK 10.2 hard-float (`arm-riscos-gnueabihf-gcc`), OSLib, a **static** ELF
(filetype `&E1F`; needs the ARMEABISupport module on the target, like the
other projects).

```
export PATH="$HOME/gccsdk/cross/bin:$HOME/gccsdk/env:$PATH"
make            # -> build/riscos/!Hotspot/  (copy the whole folder across)
```

Files in `build/riscos/!Hotspot` carry RISC OS file type suffixes
(`!RunImage,e1f`, `!Run,feb`, `!Sprites,ff9`, `!Help,fff`, `Choices,fff`) so
the types survive a copy over SMB or onto a FAT USB stick.

```
make dist                  # -> build/riscos/Hotspot-<version>.zip
tools/deliver-nas.sh       # -> smb://nas1.local/RISCOS/Development/Hotspot
```

The version number is `APP_VERSION` in `src/ro.h`. The zip has plain names;
the types, load/exec addresses and attributes are in the Acorn "ARC0" extra
field exactly as a zip made on RISC OS has them. `deliver-nas.sh` copies the
application folder, the zip and the ReadMe, fetches them back and compares.

## Testing without the hardware

```
make test
```

runs, all under AddressSanitizer + UBSan:

1. **Unit tests** (`test/test_core.c`): the utilities, HTML/JSON readers, the
   parsers against sample replies, every request builder, the row layout.
2. **Client integration** (`test/it_client.c`) against `test/mock_wpsd.py`, a
   stand-in dashboard that reproduces the real templates' markup and quirks
   (the PHP pill helper's stray quote, `&nbsp B` without a semicolon, `\/`
   in the JSON, a 150 KB admin page around a manager's reply…): polling,
   every action, wrong password, nothing listening, slow hotspot, HTTP
   errors, chunked / no-length / trickled / short bodies.
3. **HTTP framing at every segmentation** (`test/split_server.py`): canned
   replies cut at every byte boundary (~560 cases) — header terminators and
   chunk sizes split across TCP segments, 100-Continue, bare-LF headers.
4. **The whole front end on a fake desktop** (`test/ui_sim.c`,
   `test/fakewimp.c`): the real `ro_*.c` compiled unchanged against the real
   OSLib headers, with the ~25 OSLib functions it calls replaced by a model
   Wimp. A scripted "user" clicks what the program actually drew (the
   rectangles it handed to `Wimp_PlotIcon`), so a mismatch between the drawing
   and the hit-testing shows up as a click that does nothing. Covers
   first-run Choices (starting from the shipped defaults), drawing,
   Pause/Resume, every tab, BrandMeister, the DMR networks and TGIF on a
   gateway hotspot, link dialogues and remembered targets, the search for a
   hotspot after the address has moved (declining, accepting, finding
   nothing, stopping), confirmations, menus, scrolling, close/reopen
   (polling stops and resumes) and Quit.

   The fake desktop models what a review against the real Wimp source
   found the first version was blind to, so those mistakes now fail the
   tests: `Wimp_ReportError` flag/register misuse, a window larger than its
   extent being trimmed, keys going only to the window that owns the caret,
   `Tab`/`Return` navigation between writable fields, and any dialogue text
   too wide for its icon.

5. **Fuzzing** (`test/fuzz_parsers.c`): ~15,000 mutated copies of the sample
   replies per run (more with a different seed) through every parser, each
   input malloc'd to its exact length so a read past the end is caught.
6. **Hostile replies** (`it_client --hostile`): an endless header block, an
   absurd chunk size, a Content-Length far beyond what is sent, a 3 MB body
   into a 4 KB cap, a connection reset mid-body, a server that never answers,
   not-HTTP-at-all.

What the host tests **cannot** prove — see *Unverified* below.

### Checking a real hotspot from Linux

```
make probe
build/host/hs_probe HOTSPOT_ADDRESS [-p PORT] [-w ADMIN_PASSWORD] [-d]
```

fetches the same pages the program does (read-only: nothing it sends changes
anything on the hotspot), prints how each request went and what the parsers
made of it, and renders the Status and last-heard views as text; `-a` shows
every tab as the window would, `-d` adds the full diagnostics report. This is
the quickest way to find out whether the installed dashboard's markup matches
what the parsers expect. It has been run against a real WPSD hotspot
(a duplex MMDVM_HS_Dual_Hat with BrandMeister and TGIF through DMRGateway,
and YSF): radio, modes, networks, YSF link, hardware cards and last heard all
parsed.

## Unverified (needs the real hotspot / real Wimp)

* The HTML the *installed* WPSD serves. The parsers were built from the
  current dashboard source; an older or modified dashboard may differ. If a
  part of the display stays empty, `System → Save diagnostics` writes the
  exact requests and replies (password omitted) — that is what to send back.
* That the real Wimp likes every flag combination (a fake desktop only checks
  the program's own logic). The window, icon, menu and dialogue definitions
  were checked against the real Wimp source where possible.
* The look: layout was designed from the drawing coordinates, never seen.
* TGIF's current talkgroups: that hotspot's public `tgif_links.php` returned
  an empty page (HTTP 200, no body), so the TGIF section there shows a single
  Link/Unlink row instead of one per timeslot, and whether the dashboard's
  TGIF manager accepts the requests is unknown until it is tried.
* The DMR network list and BrandMeister's page need the Admin login, so they
  have only been tested against the mock.

## Ideas not done yet

WPSD *profiles* (switch the whole configuration: `POST /admin/profile_manager.php`
with `configs=<name>&restore_config=…`), a picker for the YSF reflector list,
XLX module switching, TLS (AcornSSL is already proven in `ssl-test`), a
"live caller" iconbar indicator.
