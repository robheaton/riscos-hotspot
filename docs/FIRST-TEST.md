# First test on the RISC OS machine (Hotspot 0.06)

Everything below is what to do the first time, step by step. The program has
never run on real RISC OS yet — only against a mock hotspot and a fake desktop
on the Linux machine, and (read-only, parsing only) against the real hotspot's
public pages from Linux — so the point of this round is to find out what the
real desktop does differently. The same steps are in `ReadMe,fff` beside the
application on the NAS.

## Before you start

* The RISC OS machine needs the **ARMEABISupport** module (the same as for the
  other native programs; the program is a static ELF, so **SharedLibs is not
  needed**) and a working network connection to the hotspot.
* The hotspot is at **10.0.0.27**; the program starts with that address. You
  need the **password for its Admin pages** (the user name is `pi-star` unless
  you changed it). Looking at the hotspot needs no login; changing anything
  does.
* The **Reboot** and **Shut down** buttons really do reboot / shut down the
  hotspot (they ask first).

## Steps

1. Copy the whole `!Hotspot` folder to a local disc on the RISC OS machine
   (it is on the NAS in `Development/Hotspot/!Hotspot`; the zip
   `Hotspot-0.06.zip` beside it holds the same). Run it from the local copy.
2. Open the folder in the Filer. `!Hotspot` should show a blue radio-mast
   icon (if it shows a plain application icon, note that — it means
   `!Sprites` was not picked up).
3. **Double-click `!Hotspot`.** An icon "Hotspot" appears on the icon bar, the
   main window opens, and a **Hotspot choices** window opens on top of it
   (first run).
   *If nothing appears, or an error box shows, write down the exact words
   (and if it says it cannot run the file, check `Alias$@RunType_E1F` and the
   ARMEABISupport module).*
4. In the Choices window check **Hotspot address** says 10.0.0.27, **Port** 80,
   **User name** `pi-star`; type the **Password**; click **Save**. (Tab or
   Return moves from box to box; Return in the last box saves. The password
   shows as asterisks; if it shows as plain text, note that.)
5. The main window should say **Connected.** within a few seconds and fill
   with: *Radio* (IDLE, 438.780 MHz, MMDVM_HS_Dual_Hat), *Modes Enabled*
   (DMR and YSF, each with a **Pause** button), *Network Status*, *DMR Status*,
   *DMR Masters* (BM 2341 United Kingdom, TGIF Network), *YSF Status* (Link
   GB-NWFG2), and *Hotspot* (CPU load, temperature…). The buttons are against
   the right-hand edge of the window, visible without scrolling.
   *If it says "Not connected to 10.0.0.27: …", write down the message and try
   step 12. If it says Connected but a list is empty, go to "If something is
   empty" below.*
6. Click the **Heard** tab: the last heard list appears (UTC time, callsign,
   mode, target). When anyone is transmitting the top row turns green and says
   "on air".
7. Click the **DMR** tab. You should see **DMR networks** (BM 2341 United
   Kingdom and TGIF Network, each with **Disable**), **BrandMeister** (static
   talkgroups with Link/Drop/Delete, Add TG…, Drop QSO, Drop dynamic) and
   **TGIF** (a row with Link… / Unlink; on this hotspot the public page that
   says which talkgroup is linked returned nothing, so it will probably say
   "current links not shown"). Watch the *Last action* line under the title and
   try, one at a time:
   * **Disable** next to TGIF Network, wait ~5 seconds (it becomes **Enable**),
     then **Enable**;
   * **Link** or **Drop** on a BrandMeister talkgroup you do not mind changing;
   * **Link…** on the TGIF row, type a talkgroup, click Link.
   (If BrandMeister says "No BrandMeister API key defined", that is the
   hotspot telling you.)
8. Click the **Links** tab: *YSF reflector — linked to GB-NWFG2*. Click
   **Link…**, type a YSF reflector (for example 00001), click Link; after a few
   seconds the row shows the new link and a *Recent YSF reflectors* row appears
   with a button for it (click it: the same reflector is linked again in one
   go). Put GB-NWFG2 back with Link… when you have finished.
9. Click the **YSF** tab. This is the hotspot's own list of YSF reflectors and
   FCS rooms (read from the hotspot when the tab comes up, so it needs the
   Admin login). At the top: what YSF is linked to now, **Number…** (type one,
   as on the Links tab), **Unlink**, and the ones you linked recently. Click
   **Search…**, type part of a name, number or place (such as `calling`,
   `italy` or `00010`) and click Search: the list narrows to what matches, and
   **Show all** brings the rest back. Click **Link** beside a reflector: after
   a few seconds *Linked to* shows it and it appears under *Recently linked*.
   Put GB-NWFG2 back when you have finished.
10. Back on **Status**, click **Pause** next to DMR or YSF; the button should
   turn into **Resume** and *Last action - Pause …* should appear. Click
   **Resume**.
11. Press the **Menu** mouse button over the window (Refresh, Modes, DMR, Links,
    System, View, Find hotspot…, Save diagnostics, Choices…) and over the icon
    bar icon (Info, Help…, Find hotspot…, Choices…, Quit; clicking the icon itself opens the window; Help… opens the help file in your editor) and check they
    open and do what they say. Move the pointer onto the arrow beside Info: the
    Program information window opens beside the menu, as in other programs. After clicking in the window, **F5** refreshes.
12. **If the hotspot's address changes**, or to try it: click **Find
    hotspot…**. The box starts with `10.0.0.`; click **Search**. After a few
    seconds, a hotspot found at the address already in use is reported as
    such; one found elsewhere is offered ("Found a hotspot at … Use this
    address?" — Continue uses it). To see the whole thing work, change the address
    in **Choices…** to a wrong one (say 10.0.0.99), Save, wait for "Not
    connected to 10.0.0.99", click the **Find hotspot…** button beside it,
    Search, and click Continue when it offers 10.0.0.27.
13. **System** tab → **Save**: a box says where the diagnostics report was
    saved (in your Choices folder, file `Diagnostics`). Copy that file to the
    NAS.

## If something is empty or wrong

The **Diagnostics** file (step 13) contains, for every request the program
makes, the URL, HTTP status, headers, and the first 24 KB of the reply — with
the password left out. Send it back with a note of *which* part of the window
was wrong, and the parsers can be fixed against the real thing.

Also useful: a screenshot of the window, and the exact text of any error box.
The System tab also lists, in plain words, how each request went ("Status:
HTTP 200, 4.7 KB - 27 items in 7 sections").
