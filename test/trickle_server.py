#!/usr/bin/env python3
"""
trickle_server.py - a "hotspot" that never finishes answering.

Accepts connections, reads the request, sends the headers of a long reply and
then one more byte every 200 ms for a minute. Whatever probes it keeps
receiving data, so an inactivity timeout never fires: only an overall
deadline gets rid of it (the search for a hotspot has to).

usage: trickle_server.py --bind ADDRESS --port PORT [--quiet]
"""

import argparse
import socket
import threading
import time


def serve(conn):
    try:
        conn.settimeout(5)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(4096)
            if not chunk:
                return
            data += chunk
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                     b"Content-Length: 100000\r\nConnection: close\r\n\r\n")
        for _ in range(300):
            time.sleep(0.2)
            conn.sendall(b" ")
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(16)
    print("PORT=%d" % args.port, flush=True)

    while True:
        conn, _ = srv.accept()
        threading.Thread(target=serve, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
