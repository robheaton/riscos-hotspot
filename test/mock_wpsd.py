#!/usr/bin/env python3
"""
mock_wpsd.py - a stand-in for a WPSD hotspot's web dashboard.

It serves the handful of URLs the RISC OS Hotspot client uses, with markup
copied from the real dashboard templates (WPSD-WebCode: mmdvmhost/radioinfo.php,
mmdvmhost/repeaterinfo.php, includes/hw_info.php, api/index.php,
admin/bm-manager.php, admin/system_api.php and the instant-mode / link
manager pages), including their quirks: the stray quote the PHP pill helper
leaves in the class attribute, "&nbsp B" without a semicolon, a 150KB admin
page wrapped around a manager's reply, \\/ escaping in the JSON.

State is kept in memory so actions have visible effects (pause a mode, link
a reflector, add a talkgroup...). Behaviour switches (chunked replies, slow
replies, a hotspot that drops the connection on reboot...) can be flipped at
run time with  GET /__mock/set?name=value  and the requests it has seen are
available from GET /__mock/log.

    mock_wpsd.py [--port N] [--password P]      serve (prints PORT=<n>)
    mock_wpsd.py --dump-fixtures DIR            write sample replies to DIR

Python 3 standard library only.
"""

import argparse
import base64
import json
import random
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


# ---------------------------------------------------------------------------
# The hotspot
# ---------------------------------------------------------------------------

class Hotspot:
    def __init__(self, password="raspberry"):
        self.lock = threading.Lock()
        self.password = password
        self.callsign = "M1ABC"
        self.dmr_id = "2345678"
        self.duplex = False
        self.modes = {"D-Star": True, "DMR": True, "YSF": True,
                      "P25": False, "NXDN": False, "POCSAG": False}
        self.paused = set()
        self.dmr_master = "BM 2341 United Kingdom"
        self.dstar_link = "REF001 C"
        self.ysf_link = "UK-Calling"
        self.p25_tg = ""
        self.nxdn_tg = ""
        self.bm_ok = True
        self.bm_notice = "Notice! No BrandMeister API key defined."
        self.bm_static = [
            {"tg": 91, "slot": 0, "linked": True, "name": "Worldwide"},
            {"tg": 235, "slot": 0, "linked": True, "name": "UK Wide"},
            {"tg": 9990, "slot": 0, "linked": False, "name": "Parrot"},
        ]
        self.bm_dynamic = [
            {"tg": 2341, "slot": 0, "name": "UK Calling",
             "timeout": "07:25:11 (3:21)"},
        ]
        # A DMRGateway hotspot: BrandMeister and TGIF side by side.
        self.gateway = False
        self.dmrnets = [
            {"id": "net1", "name": "BM 2341 United Kingdom", "enabled": True},
            {"id": "net4", "name": "TGIF Network", "enabled": True},
        ]
        self.tgif_on = False
        self.tgif = {1: 0, 2: 91}
        self.radio = "IDLE"
        self.heard_seq = 0
        self.log = []
        self.opts = {
            "chunked": 0,        # use Transfer-Encoding: chunked
            "delay_ms": 0,       # wait before answering
            "trickle": 0,        # write the body in 100 byte pieces, N ms apart
            "reboot_drop": 0,    # close the socket on reboot/shutdown
            "force_status": 0,   # answer every non-mock request with this code
            "no_length": 0,      # no Content-Length, read until close
            "early_eof": 0,      # promise more body than is sent
            "legacy": 0,         # serve an older, table-style status sidebar
            "ctrl_chars": 0,     # new lines, tabs and colour codes inside reply text
        }

    # -- helpers -----------------------------------------------------------

    def enabled(self, mode):
        return self.modes.get(mode, False)

    def is_paused(self, mode):
        return mode in self.paused

    # -- last heard --------------------------------------------------------

    def heard(self, limit=15, names=True, country=True):
        rows = []
        base = [
            ("DMR Slot 2", "G4ABC", "Alice", "England", "TG 91", "Net", "4.2", "0%"),
            ("D-Star", "M0XYZ", "Bob", "England", "CQCQCQ", "RF", "12.5", "0%"),
            ("YSF", "2E0DEF", "Carol", "England", "UK-Calling", "Net", "7.1", "1%"),
            ("DMR Slot 2", "OK1ABC", "Petr", "Czech Republic", "TG 235", "Net", "2.0", "0%"),
            ("DMR Slot 2", "9M2FZY", "Zul", "Malaysia", "TG 91", "Net", "Timeout", "0%"),
            ("DMR Slot 2", "W0CHP", "Chip", "United States", "TG 9990", "RF", "3.3", "0%"),
            ("D-Star", "DL1ABC", "Hans", "Germany", "CQCQCQ", "Net", "9.9", "0%"),
            ("DMR Slot 2", "VK1KCM", "Kim", "Australia", "TG 91", "Net", "5.5", "2%"),
        ]
        t0 = 7 * 3600 + 12 * 60 + 11
        for i in range(limit):
            m, call, name, ctry, tgt, src, dur, loss = base[i % len(base)]
            when = t0 - 31 * i + self.heard_seq
            ts = "2026-10-04 %02d:%02d:%02d" % (when // 3600 % 24,
                                                 when // 60 % 60, when % 60)
            row = {"time_utc": ts, "mode": m, "callsign": call}
            if names:
                row["name"] = name
            if country:
                row["country"] = ctry
            row["callsign_suffix"] = ""
            row["target"] = tgt
            row["src"] = src
            row["duration"] = "" if (i == 0 and self.radio.startswith("RX")) else dur
            row["loss"] = loss
            rows.append(row)
        return rows


# ---------------------------------------------------------------------------
# Page generators (mirroring the PHP templates)
# ---------------------------------------------------------------------------

def php_json(obj):
    """json_encode() as PHP does it by default: \\u escapes, \\/ escaped."""
    return json.dumps(obj, ensure_ascii=True, separators=(",", ":")).replace("/", "\\/")


def output_pill(label, status_class, icon_class="", value="", title=""):
    if not icon_class:
        icon_class = {
            "active": "fa fa-check-circle text-success",
            "paused": "fa fa-pause-circle text-warning",
            "error": "fa fa-exclamation-triangle text-danger",
            "inactive": "fa fa-circle-o text-muted",
        }.get(status_class, "fa fa-info-circle")
    s = "<div class='status-pill %s' title=\"%s\">" % (status_class, title)
    s += "<span>%s</span>" % label
    s += "<div class='pill-data'>"
    if value != "":
        s += "<span class='pill-value'>%s</span> " % value
    s += "<i class='%s'></i>" % icon_class
    s += "</div></div>"
    return s


def page_radioinfo(h):
    cells = [
        ("Radio Status", h.radio),
        ("TX/RX Freq.", "438.800 MHz"),
        ("Radio Mode", "Simplex"),
        ("Modem Port", "/dev/ttyACM0"),
        ("Modem Speed", "115,200 bps"),
        ("TCXO Freq.", "12.2880 MHz"),
        ("Modem Type", "MMDVM_HS_Hat-v1.6.1 20230606 14.7456MHz ADF7021 FW by CA6JAU"),
    ]
    o = ['<div class="divTable">', '  <div class="divTableBody">',
         '    <div class="divTableRow center">']
    for i, (k, _) in enumerate(cells):
        if i == 0:
            o.append('      <div class="divTableHeadCell noMob" style="width:250px;">%s</div>' % k)
        else:
            o.append('      <div class="divTableHeadCell noMob">%s</div>' % k)
    o.append("    </div>")
    o.append('    <div class="divTableRow center">')
    state = h.radio
    if state.startswith("TX"):
        o.append('        <div class="divTableCell middle cell_content" style="background:#d11141; color:#ffffff; font-weight:bold;padding:2px;">%s</div>' % state)
    elif state.startswith("RX"):
        o.append('        <div class="divTableCell middle active-mode-cell" style="font-weight:bold;padding:2px;">%s</div>' % state)
    elif state == "OFFLINE":
        o.append('        <div class=\'error-state-cell divTableCell middle cell_content\' style="font-weight:bold;padding:2px;">OFFLINE</div>')
    else:
        o.append('        <div class="divTableCell middle cell_content" style="font-weight:bold;padding:2px;">%s</div>' % state)
    for _, v in cells[1:]:
        o.append('      <div class="divTableCell cell_content middle noMob" style="background: inherit;">%s</div>' % v)
    o.append("    </div>")
    o.append("  </div>")
    o.append("</div>")
    return "\n".join(o) + "\n"


def page_hwinfo(h):
    return (
        '<div id="hwInfoTable" class="dashboard-header-stats">\n'
        "    \n"
        '    <div class="stat-card">\n'
        '        <span class="stat-label">CPU Load</span>\n'
        '        <div class="stat-value">\n'
        '            <a class="tooltip" href="#">12%<span><strong>Hardware:</strong> Raspberry Pi Zero 2 W Rev 1.0<br />'
        "<strong>Platform:</strong> Pi Zero 2 W<br />"
        '<strong>OS:</strong> Raspbian GNU/Linux 12 "bookworm" (release ver. 12.5)<br />'
        "<strong>Linux Kernel:</strong> 6.1.21-v8+<br />"
        "<strong>Uptime:</strong>  3 days, 4 hours, 12 minutes</span></a>\n"
        "        </div>\n"
        "    </div>\n"
        '    <div class="stat-card">\n'
        '        <span class="stat-label">CPU Temp</span>\n'
        '        <div class="stat-value">\n'
        '            <a class="tooltip" href="#">130&deg;F / 54&deg;C<span><strong>CPU Temperature</strong></span></a>\n'
        "        </div>\n"
        "    </div>\n"
        '    <div class="stat-card">\n'
        '        <span class="stat-label">RAM Usage</span>\n'
        '        <div class="stat-value">\n'
        '            <a class="tooltip" href="#">96.5 MB / 427.3 MB<span><strong>Used:</strong> 22.58%<br><strong>Free:</strong> 330.8 MB</span></a>\n'
        "        </div>\n"
        "    </div>\n"
        '    <div class="stat-card">\n'
        '        <span class="stat-label">Disk Usage</span>\n'
        '        <div class="stat-value">\n'
        '            <a class="tooltip" href="#">1.9 GB / 14.5 GB<span><strong>Used:</strong> 13.1%</span></a>\n'
        "        </div>\n"
        "    </div>\n"
        '    <div class="stat-card">\n'
        '        <span class="stat-label">Net Traffic</span>\n'
        '        <div class="stat-value">\n'
        '            <a class="tooltip" href="#">N/A<span><strong>Total Network Traffic</strong><br />Collecting data...<br>(Interface: wlan0)</span></a>\n'
        "        </div>\n"
        "    </div>\n"
        "</div>\n"
    )


def page_repeaterinfo(h):
    o = []
    o.append('<div class="sidebar-section-title">Modes Enabled</div>')
    o.append('<div class="sidebar-status-grid" id="rptInfoTable">')
    for mode, key in (("D-Star", "D-Star"), ("DMR", "DMR"), ("YSF", "YSF"),
                      ("P25", "P25"), ("NXDN", "NXDN"), ("POCSAG", "POCSAG")):
        if h.is_paused(mode):
            o.append("    " + output_pill(mode, "paused"))
        elif h.enabled(mode):
            o.append("    " + output_pill(mode, "active"))
        else:
            o.append("    " + output_pill(mode, "inactive"))
    o.append("</div>")
    o.append("")
    o.append("<br />")
    o.append("")
    o.append('<div class="sidebar-section-title">Network Status</div>')
    o.append('<div class="sidebar-status-grid">')
    for mode, label in (("D-Star", "D-Star Net"), ("DMR", "DMR Net"),
                        ("YSF", "YSF Net"), ("P25", "P25 Net"),
                        ("NXDN", "NXDN Net"), ("POCSAG", "POCSAG Net")):
        if h.is_paused(mode):
            o.append("    " + output_pill(label, "paused"))
        elif h.enabled(mode):
            o.append("    " + output_pill(label, "active"))
        else:
            o.append("    " + output_pill(label, "inactive"))
    o.append("    " + output_pill("APRS Net", "inactive"))
    o.append("</div>")
    o.append("")
    o.append("<br />")

    if h.enabled("D-Star") or h.is_paused("D-Star"):
        o.append('    <div class="sidebar-section-title">D-Star Status</div>')
        o.append('    <div class="sidebar-status-grid">')
        o.append('        <div class="status-pill active" style="grid-column: span 2;"><span>RPT1</span><span class="pill-value">%s&nbsp B</span></div>' % h.callsign)
        o.append('        <div class="status-pill active" style="grid-column: span 2;"><span>RPT2</span><span class="pill-value">%s&nbsp G</span></div>' % h.callsign)
        o.append("        " + output_pill("Public", "active' style='grid-column: span 2;'", "fa fa-globe", "On"))
        o.append("    </div>")
        o.append("")
        o.append('    <div class="sidebar-section-title related">D-Star Network</div>')
        o.append('    <div class="sidebar-status-grid">')
        if h.is_paused("D-Star"):
            o.append("<div class='status-pill paused' style='grid-column: span 2;'><span>D-Star</span><span class='pill-data'><span class='pill-value'>Mode Paused</span><i class='fa fa-pause-circle'></i></span></div>")
        else:
            o.append("<div class='status-pill active' style='grid-column: span 2;' title=\"%s\"><span>Link</span><span class='pill-value'>%s</span></div>" % (h.dstar_link, h.dstar_link))
        o.append("<div class='status-pill active' style='grid-column: span 2;'><span>ircDDB</span><span class='pill-value'>ircv4.openquad.net</span></div>")
        o.append("    </div>")
        o.append("    <br />")

    if h.enabled("DMR") or h.is_paused("DMR"):
        master = "Mode Paused" if h.is_paused("DMR") else h.dmr_master
        o.append('    <div class="sidebar-section-title">DMR Status</div>')
        o.append('    <div class="sidebar-status-grid">')
        o.append("        " + output_pill("TS2", "active", "fa fa-check", "On"))
        o.append("        " + output_pill("Public", "active", "fa fa-check", "Semi"))
        o.append('        <div class="status-pill active"><span>ID</span><span class="pill-value">%s</span></div>' % h.dmr_id)
        o.append('        <div class="status-pill active"><span>CC</span><span class="pill-value">1</span></div>')
        o.append("    </div>")
        o.append("")
        if h.gateway and not h.is_paused("DMR"):
            o.append('    <div class="sidebar-section-title related">DMR Masters</div>')
            o.append('    <div class="sidebar-status-grid">')
            for net in h.dmrnets:
                style = "active" if net["enabled"] else "paused"
                o.append("        <div class='status-pill %s' style='grid-column: span 2;' title=\"%s\"><span>%s</span><i class='fa fa-link'></i></div>" % (style, net["name"], net["name"]))
            o.append("    </div>")
        else:
            o.append('    <div class="sidebar-section-title related">DMR Master</div>')
            o.append('    <div class="sidebar-status-grid">')
            o.append("        <div class='status-pill active' style='grid-column: span 2;' title=\"%s\"><span>%s</span><i class='fa fa-link'></i></div>" % (master, master))
            o.append("    </div>")
        o.append("    <br />")

    if h.enabled("YSF") or h.is_paused("YSF"):
        state = "" if h.is_paused("YSF") else (" [Linked]" if h.ysf_link != "Not Linked" else "")
        o.append('    <div class="sidebar-section-title">YSF Status%s</div>' % state)
        o.append('    <div class="sidebar-status-grid">')
        o.append("        " + output_pill("Public", "active' style='grid-column: span 2;'", "fa fa-globe", "On"))
        if h.is_paused("YSF"):
            o.append("        " + output_pill("Status", "paused", "fa fa-pause", "Paused"))
        else:
            o.append("        <div class='status-pill active' style='grid-column: span 2;' title=\"\"><span>Link</span><div class='pill-data'><span class='pill-value'>%s</span><i class='fa fa-link'></i></div></div>" % h.ysf_link)
        o.append("    </div>")
        o.append("    <br />")

    if h.enabled("P25") or h.is_paused("P25"):
        o.append('    <div class="sidebar-section-title">P25 Status</div>')
        o.append('    <div class="sidebar-status-grid">')
        o.append("        <div class='status-pill active' style='grid-column: span 2;'><span>NAC</span><div class='pill-data'><span class='pill-value'>293</span></div></div>")
        if h.is_paused("P25"):
            o.append("        " + output_pill("P25 Net", "paused' style='grid-column: span 2;'", "fa fa-pause", "Paused"))
        elif h.p25_tg:
            o.append("        <div class='status-pill active' style='grid-column: span 2;'><span>Link</span><div class='pill-data'><span class='pill-value'>TG %s</span><i class='fa fa-link'></i></div></div>" % h.p25_tg)
        else:
            o.append("        " + output_pill("P25 Net", "inactive' style='grid-column: span 2;'", "fa fa-unlink", "Unlinked"))
        o.append("    </div>")
        o.append("    <br />")

    if h.enabled("NXDN") or h.is_paused("NXDN"):
        o.append('    <div class="sidebar-section-title">NXDN Status</div>')
        o.append('    <div class="sidebar-status-grid">')
        o.append('        <div class="status-pill active" style="grid-column: span 2;"><span>RAN</span><div class="pill-data"><span class="pill-value">1</span></div></div>')
        if h.is_paused("NXDN"):
            o.append("        " + output_pill("Net", "paused' style='grid-column: span 2;'", "fa fa-pause", "Paused"))
        elif h.nxdn_tg:
            o.append("        <div class='status-pill active' style='grid-column: span 2;'><span>Link</span><div class='pill-data'><span class='pill-value'>TG %s</span><i class='fa fa-link'></i></div></div>" % h.nxdn_tg)
        else:
            o.append("        " + output_pill("Net", "inactive' style='grid-column: span 2;'", "fa fa-unlink", "Unlinked"))
        o.append("    </div>")
        o.append("    <br />")

    # The APRS block repeats mode names as plain flags - they must NOT be
    # mistaken for the mode list.
    o.append('    <div class="sidebar-section-title">APRS Gateway</div>')
    o.append('    <div class="sidebar-status-grid">')
    o.append("        <div class='status-pill active' style='grid-column: span 2;' title='Pool: euro.aprs2.net'><span>Pool</span><div class='pill-data'><span class='pill-value'>euro.aprs2.net</span><i class='fa fa-server'></i></div></div>")
    o.append("    </div>")
    o.append('    <div class="sidebar-section-title related">APRS Modes</div>')
    o.append('    <div class="sidebar-status-grid">')
    o.append("        " + output_pill("DMR", "active"))
    o.append("        " + output_pill("D-Star", "inactive"))
    o.append("        " + output_pill("YSF", "active"))
    o.append("        <div class='status-pill inactive' style='grid-column: span 2;'><a href='/admin/configure.php#APRSgw' style='color:inherit;'>No more modes</a></div>")
    o.append("    </div>")
    o.append("    <br />")
    return "\n".join(o) + "\n"


def page_tgif_links(h):
    """mmdvmhost/tgif_links.php: empty unless TGIF is configured."""
    if not h.tgif_on:
        return ""

    def cell(slot):
        tg = h.tgif[slot]
        if tg == 0:
            return ('  <td align="left" style="padding: 8px;">None<span style="float:right;"></span></td>')
        return ('  <td align="left" style="padding: 8px;">TG%d<span style="float:right;">%s</span></td>'
                % (tg, {91: "Worldwide", 235: "UK Wide", 2341: "UK Calling"}.get(tg, "Talkgroup %d" % tg)))

    return (
        '<b>Active TGIF Connections</b>\n'
        '        <table>\n'
        '          <tr>\n'
        '            <th align="left" style="padding-left: 8px;"><a class=tooltip href="#">Connected to Master:<span><b>Connected Master</b></span></a></th>\n'
        '            <th align="left" style="padding-left: 8px;"><a class=tooltip href="#">Slot 1 Talkgroup<span><b>TG linked to Slot 1</b></span></a></th>\n'
        '            <th align="left" style="padding-left: 8px;"><a class=tooltip href="#">Slot 2 Talkgroup<span><b>TG linked to Slot 2</b></span></a></th>\n'
        '          </tr>\n'
        '<tr>\n'
        '  <td align="left" style="padding: 8px;white-space:normal; word-wrap:break; width:200px;">tgif.network<br /><small>'
        '(<a href="http://tgif.network/selfcare.html" target="_blank">Your HotSpot/Repeater ID: %s</a>)</small></td>'
        % h.dmr_id +
        cell(1) + cell(2) + '</tr>\n</table>\n')


DMR_CSS = """
    .dmr-wrapper { display: flex; justify-content: center; }
    .dmr-card { border-radius: 8px; }
    .dmr-net-row { display: flex; justify-content: space-between; padding: 12px 20px; }
    .dmr-net-name { font-weight: 600; }
    .dmr-switch { position: relative; width: 44px; height: 22px; }
"""


def page_dmr_man(h):
    """The DMR Network Manager card, wrapped in the admin page as the real one
    is (it only exists inside /admin/index.php?func=dmr_man)."""
    nets = [n for n in h.dmrnets]
    if not h.gateway or len(nets) < 2:
        return wrap_admin_page("", h)
    o = ["<style>", DMR_CSS, "</style>", "<script>/* class=\"dmr-net-row\" */</script>",
         '<div class="dmr-wrapper">', '    <div class="dmr-card">',
         '        <div id="dmr-nav-placeholder"></div>',
         '        <div class="dmr-header">DMR Network Manager</div>',
         '        <div class="dmr-body">', '            <div id="dmrNetManList">']
    for n in nets:
        o += ['                    <div class="dmr-net-row">',
              '                        <div class="dmr-net-name">%s</div>' % n["name"],
              '                        <label class="dmr-switch">',
              '                            <input class="dmrnetman-switch" type="checkbox" data-net-id="%s" %s>'
              % (n["id"], "checked" if n["enabled"] else ""),
              '                            <span class="dmr-slider"></span>',
              '                        </label>', '                    </div>']
    o += ['            </div>', '        </div>', '    </div>', '</div>']
    return wrap_admin_page("\n".join(o), h)


def page_repeaterinfo_legacy(h):
    """An older dashboard's sidebar: tables, not status pills."""
    return (
        '<div class="divTable"><div class="divTableBody">\n'
        '<div class="divTableRow"><div class="divTableHeadCell">Mode Status</div></div>\n'
        '<div class="divTableRow"><div class="divTableCell">DMR</div>'
        '<div class="divTableCell">Enabled</div></div>\n'
        '<div class="divTableRow"><div class="divTableCell">YSF</div>'
        '<div class="divTableCell">Disabled</div></div>\n'
        '<div class="divTableRow"><div class="divTableHeadCell">Network Status</div></div>\n'
        '<div class="divTableRow"><div class="divTableCell">DMR Master</div>'
        '<div class="divTableCell">BM_2341_United_Kingdom</div></div>\n'
        '</div></div>\n')


BM_CSS = """
    .bm-wrapper { display: flex; justify-content: center; }
    .bm-card { background-color: #fff; border-radius: 8px; }
    .bm-alert { padding: 20px; margin: 20px; text-align: center; }
    .bm-alert-error { background-color: #d11141; color: #fff; }
    .bm-list-row { display: flex; padding: 8px; }
    .bm-col-tg { flex: 1; } .bm-col-ts { flex: 1; } .bm-col-name { flex: 3; }
    .bm-col-timeout { flex: 2; } .bm-col-action { flex: 2; }
    .bm-info-bar { padding: 10px; }
"""


def page_bm(h):
    o = ["<style>", BM_CSS, "</style>", "<script>",
         "document.addEventListener(\"DOMContentLoaded\", function() { /* class=\"bm-list-row\" */ });",
         "</script>"]
    if not h.bm_ok:
        o += ['    <div class="bm-wrapper">',
              '        <div class="bm-card">',
              '            <div id="bm-nav-placeholder"></div>',
              '            <div class="bm-header">BrandMeister Manager</div>',
              '            <div class="bm-alert bm-alert-error">',
              "                <strong>%s <a href=\"https://news.brandmeister.network/introducing-user-api-keys/\" target=\"new\" alt=\"BM API Keys\">BM API Key Announcement</a>; then <a href=\"/admin/advanced/fulledit_bmapikey.php\">Enter your API Key</a>.</strong>" % h.bm_notice,
              "            </div>", "        </div>", "    </div>"]
        return "\n".join(o) + "\n"

    o += ['    <script type="text/javascript" src="/js/bm-manager.js"></script>',
          '    <div class="bm-wrapper">', '        <div class="bm-card">',
          '            <div id="bm-nav-placeholder"></div>',
          '            <div class="bm-header">BrandMeister Manager</div>',
          '            <div class="bm-info-bar">',
          '                 ID: <a href="https://brandmeister.network/#/device/%s" target="_blank"><strong>%s</strong></a>' % (h.dmr_id, h.dmr_id),
          "                 &nbsp;&bull;&nbsp; Connected To: <strong>BM_2341_United_Kingdom</strong>",
          '                 &nbsp;&bull;&nbsp; <a href="https://w0chp.radio/brandmeister-talkgroups/" target="_blank">Full Talkgroup List</a>',
          "            </div>",
          '            <h3 class="bm-section-title">Static Talkgroups</h3>',
          '            <form id="bmm-tg-static-form" action="/admin/bm-manager.php" method="POST">',
          '                <div class="bm-list-container">',
          '                    <div class="bm-list-header">',
          '                        <div class="bm-col-tg">Talkgroup</div>',
          '                        <div class="bm-col-ts">Slot</div>',
          '                        <div class="bm-col-name">Name</div>',
          '                        <div class="bm-col-action">Actions</div>',
          "                    </div>"]
    for s in h.bm_static:
        disp = 2 if s["slot"] == 0 else s["slot"]
        checked = ' checked="checked"' if s["linked"] else ""
        o += ['                    <div class="bm-list-row">',
              '                        <div class="bm-col-tg">TG %d</div>' % s["tg"],
              '                        <div class="bm-col-ts"><span class="bm-ts-badge">TS%d</span></div>' % disp,
              '                        <div class="bm-col-name">%s</div>' % s["name"],
              '                        <div class="bm-col-action">',
              '                             <label class="bm-switch">',
              '                                <input type="checkbox" id="toggle-tg%d" name="toggle-tg%d" value="ON"' % (s["tg"], s["tg"]),
              "                                    %s" % checked,
              '                                    data-tg="%d" data-slot="%d"' % (s["tg"], s["slot"]),
              '                                    class="bmm-tg-switch">',
              '                                <span class="bm-slider"></span>',
              "                            </label>",
              '                            <a class="bm-btn-drop clickloader" href="/admin/bm-manager.php?droptg=%d&amp;slot=%d" title="Delete Static TG">Delete</a>' % (s["tg"], s["slot"]),
              "                        </div>", "                    </div>"]
    o += ["                </div>",
          '                <div class="bm-add-form">',
          '                    <textarea id="add-bm-tg-list" class="bm-textarea" name="TG" placeholder="Enter Talkgroups (One per line)"></textarea>',
          '                    <input type="hidden" name="TS" value="0">',
          '                    <input type="submit" class="bm-btn clickloader" name="static-tg-add" value="Add &amp; Link">',
          "                </div>", "            </form>",
          '            <div class="bm-hint"><b>Hint:</b> You can add multiple talkgroups at once.</div>',
          '            <h3 class="bm-section-title">Dynamic Talkgroups</h3>',
          '            <div class="dynamic-tgs" data-update-url="/admin/bm-manager.php" data-update-period="15000">',
          '                <div class="bm-list-container">',
          '                    <div class="bm-list-header">',
          '                        <div class="bm-col-tg">Talkgroup</div>',
          '                        <div class="bm-col-ts">Slot</div>',
          '                        <div class="bm-col-name">Name</div>',
          '                        <div class="bm-col-timeout">Timeout</div>',
          "                    </div>"]
    if not h.bm_dynamic:
        o.append('<div class="bm-list-row" style="justify-content:center; padding:20px; opacity:0.6;">No Dynamic Talkgroups Linked</div>')
    for d in h.bm_dynamic:
        disp = 2 if d["slot"] == 0 else d["slot"]
        o += ['                        <div class="bm-list-row">',
              '                            <div class="bm-col-tg">TG %d</div>' % d["tg"],
              '                            <div class="bm-col-ts"><span class="bm-ts-badge">TS%d</span></div>' % disp,
              '                            <div class="bm-col-name">%s</div>' % d["name"],
              '                            <div class="bm-col-timeout">%s</div>' % d["timeout"],
              "                        </div>"]
    o += ["                </div>", '                <div class="bm-drop-bar">']
    if h.duplex:
        for slot in (1, 2):
            o.append('                        <input class="bm-btn-mass clickbtn" data-linkto="/admin/system_api.php?action=bm_manager&cmd=drop_qso&slot=%d" type="button" value="Drop QSO">' % slot)
            o.append('                        <input class="bm-btn-mass clickbtn" data-linkto="/admin/system_api.php?action=bm_manager&cmd=drop_dynamic&slot=%d" type="button" value="Drop All Dynamic">' % slot)
    else:
        o.append('                    <input class="bm-btn-mass clickbtn" data-linkto="/admin/system_api.php?action=bm_manager&cmd=drop_qso&slot=0" type="button" value="Drop QSO">')
        o.append('                    <input class="bm-btn-mass clickbtn" data-linkto="/admin/system_api.php?action=bm_manager&cmd=drop_dynamic&slot=0" type="button" value="Drop All Dynamic">')
    o += ["                </div>", "            </div>", "    </div>"]
    return "\n".join(o) + "\n"


def wrap_admin_page(inner, h):
    """The real admin pages are one big document with the manager's card in the
    middle; a client has to read a lot of markup before it reaches the reply."""
    css = "\n".join("    .rule%d { color: #%06x; margin: %dpx; }" % (i, i * 977 % 0xFFFFFF, i % 9) for i in range(1500))
    filler = "\n".join('    <div class="nav-item"><a href="/admin/page%d.php">Item %d</a></div>' % (i, i) for i in range(600))
    return ("<!DOCTYPE html>\n<html><head><title>WPSD Dashboard</title>\n<style>\n"
            + css + "\n</style></head>\n<body>\n" + filler + "\n" + inner
            + "\n<script>setTimeout(function() { window.location=window.location;},3000);</script>\n"
            + filler + "\n</body></html>\n")


def alert(prefix, ok, text, success_class=True):
    if ok and success_class:
        cls = "%s-alert %s-alert-success" % (prefix, prefix)
    elif ok:
        cls = "%s-alert" % prefix
    else:
        cls = "%s-alert %s-alert-error" % (prefix, prefix)
    return ('<div class="%s-wrapper"><div class="%s-card"><div id="%s-nav-placeholder"></div>'
            '<div class="%s-header">Manager</div><div class="%s-body">'
            '<div class="%s">%s</div></div></div></div>') % (prefix, prefix, prefix, prefix, prefix, cls, text)


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "lighttpd/1.4.69"
    hs = None                    # set by main()
    quiet = False

    def log_message(self, fmt, *args):
        if not self.quiet:
            sys.stderr.write("mock: " + fmt % args + "\n")

    # -- plumbing ----------------------------------------------------------

    def _auth_ok(self):
        hdr = self.headers.get("Authorization", "")
        if not hdr.startswith("Basic "):
            return False
        try:
            user, _, pw = base64.b64decode(hdr[6:]).decode().partition(":")
        except Exception:
            return False
        return user == "pi-star" and pw == self.hs.password

    def _send(self, status, body, ctype="text/html; charset=UTF-8", extra=None):
        h = self.hs
        data = body.encode("utf-8") if isinstance(body, str) else body
        if h.opts["delay_ms"]:
            time.sleep(h.opts["delay_ms"] / 1000.0)
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Connection", "close")
        if h.opts["chunked"] and status not in (204, 304):
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            pos = 0
            while pos < len(data):
                n = random.choice((1, 7, 100, 513, 1000, 4000))
                piece = data[pos:pos + n]
                pos += n
                self.wfile.write(b"%x\r\n" % len(piece) + piece + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
        else:
            if h.opts["no_length"]:
                self.end_headers()
            elif h.opts["early_eof"]:
                self.send_header("Content-Length", str(len(data) + 500))
                self.end_headers()
            else:
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
            if h.opts["trickle"]:
                for i in range(0, len(data), 100):
                    self.wfile.write(data[i:i + 100])
                    self.wfile.flush()
                    time.sleep(h.opts["trickle"] / 1000.0)
            else:
                self.wfile.write(data)
        self.wfile.flush()
        self.close_connection = True

    def _unauthorised(self):
        self._send(401, "<html><body><h1>401 Unauthorized</h1></body></html>",
                   extra={"WWW-Authenticate": 'Basic realm="WPSD Dashboard"'})

    def _redirect(self, where):
        self._send(302, "", extra={"Location": where})

    def _record(self, method, path, body):
        with self.hs.lock:
            self.hs.log.append({"method": method, "path": path,
                                "auth": self._auth_ok(), "body": body})

    # -- routing -----------------------------------------------------------

    def do_GET(self):
        self._dispatch("GET", b"")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        self._dispatch("POST", self.rfile.read(n) if n else b"")

    def _dispatch(self, method, raw):
        h = self.hs
        u = urllib.parse.urlsplit(self.path)
        path = u.path
        q = dict(urllib.parse.parse_qsl(u.query, keep_blank_values=True))
        body = raw.decode("utf-8", "replace")
        form = dict(urllib.parse.parse_qsl(body, keep_blank_values=True))

        if path.startswith("/__mock/"):
            return self._control(path, q)

        self._record(method, self.path, body)

        if h.opts["force_status"]:
            extra = None
            if h.opts["force_status"] in (301, 302):
                extra = {"Location": "https://hotspot.example/"}
            return self._send(h.opts["force_status"], "forced error", extra=extra)

        if path.startswith("/admin/") and not self._auth_ok():
            return self._unauthorised()

        with h.lock:
            if path == "/api/" or path == "/api":
                limit = int(q.get("limit", "15") or 15)
                names = q.get("names", "true") not in ("false", "0")
                country = q.get("country", "true") not in ("false", "0")
                return self._send(200, php_json(h.heard(limit, names, country)),
                                  ctype="application/json")
            if path == "/mmdvmhost/radioinfo.php":
                return self._send(200, page_radioinfo(h))
            if path == "/mmdvmhost/repeaterinfo.php":
                if h.opts["legacy"]:
                    return self._send(200, page_repeaterinfo_legacy(h))
                return self._send(200, page_repeaterinfo(h))
            if path == "/includes/hw_info.php":
                return self._send(200, page_hwinfo(h))
            if path == "/mmdvmhost/tgif_links.php":
                return self._send(200, page_tgif_links(h))
            if path == "/admin/bm-manager.php":
                return self._bm(method, q, form)
            if path == "/admin/system_api.php":
                return self._system_api(q)
            if path == "/admin/index.php" and method == "POST":
                return self._manager(q, form)
            if path == "/admin/index.php" and q.get("func") == "dmr_man":
                return self._send(200, page_dmr_man(h))

        self._send(404, "<html><body>Not found</body></html>")

    # -- control -----------------------------------------------------------

    def _control(self, path, q):
        h = self.hs
        with h.lock:
            if path == "/__mock/log":
                return self._send(200, json.dumps(h.log), ctype="application/json")
            if path == "/__mock/reset":
                h.log.clear()
                return self._send(200, "ok", ctype="text/plain")
            if path == "/__mock/set":
                for k, v in q.items():
                    if k in h.opts:
                        h.opts[k] = int(v)
                    elif k == "radio":
                        h.radio = v
                    elif k == "bm_ok":
                        h.bm_ok = bool(int(v))
                    elif k == "duplex":
                        h.duplex = bool(int(v))
                    elif k == "mode_enable":
                        h.modes[v] = True
                    elif k == "mode_disable":
                        h.modes[v] = False
                    elif k == "dmr_master":
                        h.dmr_master = v
                    elif k == "gateway":
                        h.gateway = bool(int(v))
                    elif k == "tgif":
                        h.tgif_on = bool(int(v))
                return self._send(200, "ok", ctype="text/plain")
        self._send(404, "no such control")

    # -- BrandMeister ------------------------------------------------------

    def _bm(self, method, q, form):
        h = self.hs
        if method == "POST" and "static-tg-add" in form:
            slot = int(form.get("TS", "0") or 0)
            import re
            for tg in re.findall(r"\d+", form.get("TG", "")):
                h.bm_static.append({"tg": int(tg), "slot": slot, "linked": True,
                                    "name": "TG %s" % tg})
            return self._redirect("/admin/?func=bm_man")
        if "droptg" in q:
            h.bm_static = [s for s in h.bm_static
                           if not (s["tg"] == int(q["droptg"]) and s["slot"] == int(q.get("slot", "0")))]
            return self._redirect("/admin/?func=bm_man")
        return self._send(200, page_bm(h))

    # -- system_api.php ----------------------------------------------------

    def _system_api(self, q):
        h = self.hs
        action = q.get("action", "")
        fmt = q.get("format", "json")

        def reply(obj):
            return self._send(200, php_json(obj), ctype="application/json")

        if fmt not in ("json", "text"):
            return reply({"error": "Invalid format"})
        if action in ("reboot", "shutdown"):
            if h.opts["reboot_drop"]:
                self.close_connection = True
                self.connection.close()
                return None
            return reply({"output": [], "exit_status": 0})
        if action == "restart_wpsd_services":
            if h.opts["ctrl_chars"]:
                # What a script that prints colours and a progress line does.
                return reply({"output": ["Stopping WPSD services...done",
                                         "\x1b[32mStarting\nWPSD\tservices\x1b[0m...done\r", ""],
                              "exit_status": 0})
            return reply({"output": ["Stopping WPSD services...done", "Starting WPSD services...done", ""],
                          "exit_status": 0})
        if action == "update_hostfiles":
            return reply({"output": ["Updating host files...", "Done."], "exit_status": 0})
        if action == "get_ip":
            return reply({"ip": "10.0.0.50"})
        if action == "dmrnet_set_status":
            net = q.get("dmrNet", "")
            for n in h.dmrnets:
                if n["id"] == net:
                    n["enabled"] = (q.get("netState") != "disable")
                    return reply({"commandOutput": "OK"})
            return reply({"commandOutput": "KO"})
        if action == "bm_manager":
            cmd = q.get("cmd", "")
            if cmd in ("link_static", "drop_static"):
                if "tg" not in q or "slot" not in q:
                    return reply({"error": "tg and slot params required"})
                for s in h.bm_static:
                    if s["tg"] == int(q["tg"]) and s["slot"] == int(q["slot"]):
                        s["linked"] = (cmd == "link_static")
                return reply({"success": True})
            if cmd == "drop_dynamic":
                if "slot" not in q:
                    return reply({"error": "slot param required"})
                h.bm_dynamic = []
                return reply({"success": True})
            if cmd == "drop_qso":
                if "slot" not in q:
                    return reply({"error": "slot param required"})
                return reply({"success": True})
            return reply({"error": "Bad command: " + cmd})
        return reply({"error": "Invalid action"})

    # -- manager forms -----------------------------------------------------

    def _manager(self, q, form):
        h = self.hs
        func = q.get("func", form.get("func", ""))

        if func == "mode_man":
            if not form.get("submit_mode"):
                return self._send(200, wrap_admin_page("", h))
            mode = form.get("mode_sel", "")
            act = form.get("mode_action", "")
            if not mode:
                inner = alert("imm", False, "<strong>Error:</strong> No Mode Selected.<br />Page Reloading...")
            elif act == "Pause":
                if mode in h.paused:
                    inner = alert("imm", False, "<strong>%s</strong> is already paused.<br />Page Reloading..." % mode)
                else:
                    h.paused.add(mode)
                    inner = alert("imm", True, "<strong>Paused:</strong> %s<br />Services are stopping..." % mode)
            elif act == "Resume":
                if mode not in h.paused:
                    inner = alert("imm", False, "<strong>%s</strong> is already running.<br />Page Reloading..." % mode)
                else:
                    h.paused.discard(mode)
                    inner = alert("imm", True, "<strong>Resumed:</strong> %s<br />Services are starting..." % mode)
            else:
                inner = ""
            return self._send(200, wrap_admin_page(inner, h))

        if func == "ysf_man" and "ysfMgrSubmit" in form:
            link, host = form.get("Link"), form.get("ysfLinkHost", "")
            if link == "LINK" and not host:
                inner = alert("ysf", False, "<strong>Error:</strong> No target specified. Please try again.<br>Page reloading...")
            elif link == "LINK" and host != "none":
                h.ysf_link = "Test " + host
                inner = alert("ysf", True, "<strong>Command Sent</strong><br>Linked to %s<br><br>Page reloading..." % host)
            elif link in ("LINK", "UNLINK"):
                h.ysf_link = "Not Linked"
                inner = alert("ysf", True, "<strong>Command Sent</strong><br>Unlinked<br><br>Page reloading...")
            else:
                inner = alert("ysf", False, "<strong>Error:</strong> Invalid Command (Neither Link nor Unlink).<br>Page reloading...")
            return self._send(200, wrap_admin_page(inner, h))

        if func in ("p25_man", "nxdn_man"):
            pre = func.split("_")[0]
            field = pre + "LinkHost"
            if (pre + "MgrSubmit") in form:
                link, tg = form.get("Link"), form.get(field, "")
                if link == "LINK" and tg and tg != "none":
                    setattr(h, pre + "_tg", tg)
                else:
                    setattr(h, pre + "_tg", "")
                inner = alert(pre, True, "<strong>Command Sent</strong><br>OK<br><br>Page reloading...")
                return self._send(200, wrap_admin_page(inner, h))

        if func == "ds_man" and "dstrMgrSubmit" in form:
            if not form.get("RefName") or not form.get("Letter") or not form.get("Module"):
                inner = alert("dstar", False, "<strong>Error:</strong> Invalid Input. Please try again.<br>Page reloading...", False)
            elif form.get("Link") == "LINK":
                h.dstar_link = "%-6s %s" % (form["RefName"], form["Letter"])
                inner = alert("dstar", True, "<strong>Command Sent</strong><br>Linking...<br><br>Page reloading...", False)
            else:
                h.dstar_link = "Not Linked"
                inner = alert("dstar", True, "<strong>Command Sent</strong><br>Unlinking...<br><br>Page reloading...", False)
            return self._send(200, wrap_admin_page(inner, h))

        if func == "tgif_man" and "tgifSubmit" in form:
            slot = int(form.get("tgifSlot", "2") or 2)
            if form.get("tgifAction") == "LINK" and int(form.get("tgifNumber", "0") or 0) >= 1:
                tg = int(form["tgifNumber"])
                text = "Talkgroup %d" % tg
            else:
                tg = 0
                text = "Unlink"
            h.tgif[slot] = tg
            inner = alert("tgif", True, "<strong>Command Sent</strong><br>TGIF API: %s on Slot %d<br>Status: OK<br><br>Page reloading..." % (text, slot))
            return self._send(200, wrap_admin_page(inner, h))

        return self._send(200, wrap_admin_page("", h))


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

def dump_fixtures(outdir):
    import os
    os.makedirs(outdir, exist_ok=True)

    def w(name, text):
        with open(os.path.join(outdir, name), "w", encoding="utf-8") as f:
            f.write(text)

    h = Hotspot()
    w("radioinfo_idle.html", page_radioinfo(h))
    h.radio = "RX: DMR"
    w("radioinfo_rx.html", page_radioinfo(h))
    h.radio = "TX: DMR"
    w("radioinfo_tx.html", page_radioinfo(h))
    h.radio = "OFFLINE"
    w("radioinfo_offline.html", page_radioinfo(h))
    h.radio = "IDLE"
    w("hwinfo.html", page_hwinfo(h))
    w("repeaterinfo_basic.html", page_repeaterinfo(h))
    h.paused.add("DMR")
    w("repeaterinfo_dmr_paused.html", page_repeaterinfo(h))
    h.paused.clear()
    h.modes["P25"] = True
    h.modes["NXDN"] = True
    h.p25_tg = "10100"
    h.nxdn_tg = "20"
    w("repeaterinfo_allmodes.html", page_repeaterinfo(h))
    h = Hotspot()
    w("api_lastheard.json", php_json(h.heard(8, names=False, country=False)))
    w("api_lastheard_names.json", php_json(h.heard(5, names=True, country=True)))
    h.radio = "RX: DMR"
    w("api_lastheard_rx.json", php_json(h.heard(3, names=False, country=False)))
    w("bm_page.html", page_bm(h))
    h.duplex = True
    w("bm_page_duplex.html", page_bm(h))
    h.bm_ok = False
    w("bm_notice.html", page_bm(h))
    h = Hotspot()
    h.gateway = True
    h.tgif_on = True
    w("repeaterinfo_gateway.html", page_repeaterinfo(h))
    h.dmrnets[1]["enabled"] = False
    w("repeaterinfo_gateway_tgif_off.html", page_repeaterinfo(h))
    h.dmrnets[1]["enabled"] = True
    w("tgif_links.html", page_tgif_links(h))
    h.tgif = {1: 0, 2: 0}
    w("tgif_links_none.html", page_tgif_links(h))
    w("dmr_nets.html", page_dmr_man(h))
    h.dmrnets[1]["enabled"] = False
    w("dmr_nets_tgif_off.html", page_dmr_man(h))
    h.tgif_on = False
    w("tgif_links_unused.html", page_tgif_links(h))
    h.gateway = False
    w("dmr_nets_single.html", page_dmr_man(h))
    w("api_result_dmrnet_ok.json", php_json({"commandOutput": "OK"}))
    w("api_result_dmrnet_ko.json", php_json({"commandOutput": "KO"}))
    h = Hotspot()
    w("mode_reply_paused.html", wrap_admin_page(alert("imm", True, "<strong>Paused:</strong> DMR<br />Services are stopping..."), h))
    w("mode_reply_already.html", wrap_admin_page(alert("imm", False, "<strong>DMR</strong> is already paused.<br />Page Reloading..."), h))
    w("ysf_reply_ok.html", wrap_admin_page(alert("ysf", True, "<strong>Command Sent</strong><br>Linked to YSF00001<br><br>Page reloading..."), h))
    w("ysf_reply_error.html", wrap_admin_page(alert("ysf", False, "<strong>Error:</strong> No target specified. Please try again.<br>Page reloading..."), h))
    w("dstar_reply_ok.html", wrap_admin_page(alert("dstar", True, "<strong>Command Sent</strong><br>Linking...<br><br>Page reloading...", False), h))
    w("api_result_restart.json", php_json({"output": ["Stopping WPSD services...done", "Starting WPSD services...done", ""], "exit_status": 0}))
    w("api_result_error.json", php_json({"error": "tg and slot params required"}))
    w("api_result_success.json", php_json({"success": True}))
    w("api_result_ip.json", php_json({"ip": "10.0.0.50"}))


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--password", default="raspberry")
    ap.add_argument("--bind", default="127.0.0.1",
                    help="address to listen on (the scan tests use a second loopback address)")
    ap.add_argument("--dump-fixtures", metavar="DIR")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.dump_fixtures:
        dump_fixtures(args.dump_fixtures)
        return

    Handler.hs = Hotspot(args.password)
    Handler.quiet = args.quiet
    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    srv.daemon_threads = True
    print("PORT=%d" % srv.server_address[1], flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
