#!/usr/bin/env python3
"""
split_server.py - serves one canned HTTP reply, cut into two TCP segments at
a requested byte, so the client's header and chunked-body parsing can be
exercised at every possible segmentation.

    GET /split?kind=chunked&cut=17

kinds: chunked, length, close, interim, lf_only, empty
(all of them deliver the body "Hello, World", except `empty`).

Prints PORT=<n> and serves until killed. Python 3 standard library only.
"""

import socket
import sys
import threading
import time
import urllib.parse

BODY = b"Hello, World"

REPLIES = {
    "chunked": (b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                b"5\r\nHello\r\n7;ext=1\r\n, World\r\n0\r\nX-Trailer: a\r\n\r\n"),
    "length": (b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
               b"Content-Length: 12\r\nConnection: close\r\n\r\n" + BODY),
    "close": (b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n" + BODY),
    "interim": (b"HTTP/1.1 100 Continue\r\n\r\n"
                b"HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\n" + BODY),
    "lf_only": (b"HTTP/1.1 200 OK\nContent-Length: 12\nConnection: close\n\n" + BODY),
    "empty": (b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"),
}


def hostile(conn, kind):
    """Replies a sane hotspot would never send; the client must survive them."""
    if kind == "hdrflood":
        conn.sendall(b"HTTP/1.1 200 OK\r\n")
        junk = b"X-Junk: " + b"a" * 900 + b"\r\n"
        for _ in range(200):
            conn.sendall(junk)
    elif kind == "badchunk":
        conn.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                     b"Connection: close\r\n\r\nFFFFFFFFFFFFFFFF\r\nhello\r\n0\r\n\r\n")
    elif kind == "hugelen":
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 999999999999\r\n\r\n"
                     b"Hello, World")
    elif kind == "bigbody":
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 3000000\r\n\r\n")
        block = b"x" * 10000
        for _ in range(300):
            conn.sendall(block)
    elif kind == "reset":
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial")
        time.sleep(0.05)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        b"\x01\x00\x00\x00\x00\x00\x00\x00")
    elif kind == "nothing":
        time.sleep(2)
    elif kind == "garbage":
        conn.sendall(b"NOT HTTP AT ALL\r\n\r\nrubbish")


HOSTILE = ("hdrflood", "badchunk", "hugelen", "bigbody", "reset", "nothing",
           "garbage")


def handle(conn):
    try:
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(4096)
            if not chunk:
                return
            data += chunk
        line = data.split(b"\r\n", 1)[0].decode()
        target = line.split(" ")[1]
        q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(target).query))
        if q.get("kind") in HOSTILE:
            hostile(conn, q["kind"])
            return
        reply = REPLIES[q.get("kind", "length")]
        cut = max(0, min(int(q.get("cut", "1")), len(reply)))
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if cut:
            conn.sendall(reply[:cut])
            time.sleep(0.004)
        conn.sendall(reply[cut:])
    except Exception:
        pass
    finally:
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        conn.close()


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(64)
    print("PORT=%d" % srv.getsockname()[1], flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
