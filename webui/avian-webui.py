#!/usr/bin/env python3
"""A read-only web dashboard for avian-miner.

Runs beside the miner rather than inside it. The miner's own API is a text
protocol on loopback; this polls it, keeps a little history in memory, and
serves a page that other machines on the LAN can open -- so a rig can be
watched from a desk without the hashing process also having to be an HTTP
server.

Read-only in three ways, all of them deliberate:

  * the only commands it ever sends the miner are summary, threads and
    blocks, fixed in MINER_COMMANDS; nothing from an HTTP request reaches
    the miner socket, so there is no path to seturl or quit;
  * only GET and HEAD are answered;
  * nothing is written to disk. History lives in memory and goes away with
    the process.

There is no authentication, because adding one here would invite trusting
it. Bind it where you would be comfortable publishing the numbers, and
leave the miner's own API on 127.0.0.1.

Python 3.7+, standard library only.
"""

import argparse
import json
import os
import re
import socket
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Everything this program is ever allowed to say to the miner. The privileged
# commands (seturl, quit) are absent by design and no request can add to this.
MINER_COMMANDS = ("summary", "threads", "blocks")

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "index.html")

# "kH/s" and friends. The miner scales each thread's rate for display and
# names the prefix in the key, so the key decides the multiplier rather than
# it being assumed.
RATE_KEY = re.compile(r"^([kMGTP]?)H/s$")
RATE_SCALE = {"": 1.0, "k": 1e3, "M": 1e6, "G": 1e9, "T": 1e12, "P": 1e15}


def parse_records(text):
    """The miner's reply as a list of dicts.

    One answer is "KEY=value;KEY=value|" repeated, a record per line of the
    equivalent log output, and the whole thing is NUL terminated. Values can
    hold spaces (card names do), so only ; and | are structural.
    """
    out = []
    for chunk in text.replace("\0", "").split("|"):
        if not chunk.strip():
            continue
        rec = {}
        for field in chunk.split(";"):
            if "=" not in field:
                continue
            key, _, value = field.partition("=")
            rec[key.strip()] = value
        if rec:
            out.append(rec)
    return out


def rate_of(rec):
    """A thread record's hashrate in H/s, or None if it did not report one."""
    for key, value in rec.items():
        m = RATE_KEY.match(key)
        if m:
            try:
                return float(value) * RATE_SCALE[m.group(1)]
            except ValueError:
                return None
    return None


def num(rec, key, cast=float, default=None):
    try:
        return cast(rec[key])
    except (KeyError, ValueError, TypeError):
        return default


# A miner told to bind every interface is still reachable here over loopback,
# and 0.0.0.0 is not something you can connect to.
WILDCARD = ("0.0.0.0", "::", "*")


def api_bind_address(value):
    """The address a miner given this `api-bind` will listen on, or None.

    The three forms case 'b' in cpu-miner.c accepts: ip:port, an ip alone, or
    a bare port. Mirrored rather than approximated -- a dashboard that reads a
    different port out of the same string than the miner does is worse than
    one that never looked, because it fails while appearing to know.
    """
    text = str(value).strip()

    if ":" in text:
        # The first colon, as strstr(arg, ":") finds it.
        head, _, tail = text.partition(":")
        host = head.strip()
        try:
            port = int(tail)
        except ValueError:
            return None
    elif "." in text:
        host, port = text, 4048       # an ip alone takes the default port
    else:
        try:
            port = int(text)          # a bare port is loopback
        except ValueError:
            return None
        host = "127.0.0.1"

    if port <= 0 or port > 65535:
        return None
    return ("127.0.0.1" if host in WILDCARD or not host else host, port)


class MinerConfig:
    """The miner's own -c config file, read for the `api-bind` in it.

    Re-read rather than read once, because HiveOS rewrites this file every
    time a flight sheet is applied: a rig whose API port moves should not
    also need the dashboard restarted to follow it.
    """

    def __init__(self, path):
        self.path = path
        self.stamp = None
        self.address = None
        self.error = None

    def resolve(self):
        """(host, port), or None with self.error saying why not."""
        try:
            st = os.stat(self.path)
        except OSError as exc:
            self.stamp, self.address = None, None
            self.error = "cannot read %s (%s)" % (self.path, exc.strerror or exc)
            return None

        # Size as well as mtime: a config rewritten within the same mtime tick
        # is exactly what a script regenerating it looks like.
        stamp = (st.st_mtime_ns, st.st_size)
        if stamp == self.stamp and self.address:
            return self.address
        self.stamp = stamp

        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                conf = json.load(fh)
        except (OSError, ValueError) as exc:
            self.address = None
            self.error = "cannot parse %s (%s)" % (self.path, exc)
            return None

        if not isinstance(conf, dict) or "api-bind" not in conf:
            self.address = None
            self.error = ("%s has no api-bind, so the miner is not serving "
                          "its API at all" % self.path)
            return None

        address = api_bind_address(conf["api-bind"])
        if address is None:
            self.address = None
            self.error = ("cannot read an address out of the api-bind in %s: %r"
                          % (self.path, conf["api-bind"]))
            return None

        self.error = None
        self.address = address
        return address


class MinerAPI:
    """One command, one connection -- which is the miner's API, not a choice.

    api.c closes the socket after a single reply, so there is nothing to keep
    open between polls.
    """

    def __init__(self, host, port, timeout=5.0):
        self.host = host
        self.port = port
        self.timeout = timeout

    def ask(self, command):
        if command not in MINER_COMMANDS:
            # Unreachable from HTTP; here so it stays unreachable if someone
            # later wires a new caller in.
            raise ValueError("refusing to send %r to the miner" % command)

        with socket.create_connection((self.host, self.port),
                                      timeout=self.timeout) as sock:
            sock.settimeout(self.timeout)
            sock.sendall((command + "\n").encode("ascii"))
            sock.shutdown(socket.SHUT_WR)

            parts = []
            while True:
                data = sock.recv(8192)
                if not data:
                    break
                parts.append(data)
                if b"\0" in data:
                    break

        return b"".join(parts).decode("utf-8", "replace")


class Poller:
    """The miner's current state, and as much history as we were asked to keep.

    One background thread does the polling so that a slow or dead miner
    delays nobody's page load: a request renders whatever the last successful
    poll produced, with its age, rather than blocking on the socket.
    """

    def __init__(self, api, interval, history, config=None):
        self.api = api
        self.config = config
        self.interval = interval
        self.lock = threading.Lock()
        self.total_history = deque(maxlen=history)
        self.card_history = {}       # thr_id -> deque of (t, hashrate)
        self.history_len = history
        self.state = {
            "ok": False,
            "error": "waiting for the first poll",
            "polled": 0,
            "miner": {},
            "totals": {},
            "gpu": False,
            "cards": [],
            "blocks": [],
            "history": [],
        }

    def start(self):
        thread = threading.Thread(target=self._loop, name="poller", daemon=True)
        thread.start()

    def _retarget(self):
        """Follow the miner's config file, when we were given one.

        False means the file could not be read; the poll is then skipped
        rather than aimed at a stale address, so the page says what is wrong
        with the config instead of blaming a miner that may be running fine.
        """
        if not self.config:
            return True

        address = self.config.resolve()
        if address is None:
            self._fail(self.config.error)
            return False

        if address != (self.api.host, self.api.port):
            sys.stderr.write("avian-webui: miner API is %s:%d, per %s\n"
                             % (address[0], address[1], self.config.path))
            sys.stderr.flush()
            self.api.host, self.api.port = address
        return True

    def _loop(self):
        while True:
            try:
                if self._retarget():
                    self._poll_once()
            except OSError as exc:
                # The address is in the message because the likeliest cause of
                # a refused connection is that the miner was given a different
                # --api-bind port than this was given --miner, and naming both
                # ends is what turns that from a mystery into a typo.
                self._fail("no answer from the miner at %s:%d (%s)"
                           % (self.api.host, self.api.port, exc))
            except Exception as exc:                     # noqa: BLE001
                self._fail("%s: %s" % (type(exc).__name__, exc))
            time.sleep(self.interval)

    def _fail(self, message):
        with self.lock:
            self.state["ok"] = False
            self.state["error"] = message

    def _poll_once(self):
        summary = parse_records(self.api.ask("summary"))
        threads = parse_records(self.api.ask("threads"))
        blocks = parse_records(self.api.ask("blocks"))

        if not summary:
            self._fail("miner answered 'summary' with nothing")
            return

        s = summary[0]
        now = time.time()
        gpu = any("GPU" in rec for rec in threads)

        total = num(s, "HS", float, 0.0)
        self.total_history.append((round(now, 1), total))

        cards = []
        for i, rec in enumerate(threads):
            thr = num(rec, "CPU", int, i)
            rate = rate_of(rec)
            hist = self.card_history.setdefault(
                thr, deque(maxlen=self.history_len))
            if rate is not None:
                hist.append((round(now, 1), rate))
            cards.append({
                "thr": thr,
                "gpu": num(rec, "GPU", int, None),
                "name": rec.get("CARD"),
                "vendor": rec.get("VENDOR"),
                "bus": num(rec, "BUS", int, None),
                "hashrate": rate,
                "solved": num(rec, "SOL", int, 0),
                "history": [{"t": t, "hs": v} for t, v in hist],
            })

        # The threads record carries the card's identity, so a solve can be
        # shown with the card's name even though the blocks record only has
        # to repeat it. Falling back to the live table also means a block
        # found on a card that has since been dropped by --devices still
        # names whatever the miner last knew.
        by_thr = {c["thr"]: c for c in cards}
        solves = []
        for rec in blocks:
            thr = num(rec, "CPU", int, None)
            live = by_thr.get(thr, {})
            solves.append({
                "num": num(rec, "NUM", int, None),
                "ts": num(rec, "TS", int, None),
                "height": num(rec, "HEIGHT", int, None),
                "diff": num(rec, "DIFF", float, None),
                "netdiff": num(rec, "NETDIFF", float, None),
                "thr": thr,
                "gpu": num(rec, "GPU", int, live.get("gpu")),
                "card": rec.get("CARD") or live.get("name"),
                "bus": num(rec, "BUS", int, live.get("bus")),
            })

        state = {
            "ok": True,
            "error": None,
            "polled": round(now, 1),
            "gpu": gpu,
            "miner": {
                "name": s.get("NAME"),
                "version": s.get("VER"),
                "api": s.get("API"),
                "algo": s.get("ALGO"),
                "url": s.get("URL"),
                "cpus": num(s, "CPUS", int, len(cards)),
                "uptime": num(s, "UPTIME", float, 0.0),
                "ts": num(s, "TS", int, None),
            },
            "totals": {
                "hashrate": total,
                "accepted": num(s, "ACC", int, 0),
                "rejected": num(s, "REJ", int, 0),
                # STALE postdates this dashboard's first version; absent
                # means an older miner, not zero stales.
                "stale": num(s, "STALE", int, None),
                "solved": num(s, "SOL", int, 0),
                "accmn": num(s, "ACCMN", float, None),
                "diff": num(s, "DIFF", float, None),
                "temp": num(s, "TEMP", float, None),
                "fan": num(s, "FAN", int, None),
                "freq": num(s, "FREQ", int, None),
            },
            "cards": cards,
            "blocks": solves,
            "history": [{"t": t, "hs": v} for t, v in self.total_history],
        }

        with self.lock:
            self.state = state

    def snapshot(self):
        with self.lock:
            state = dict(self.state)
        state["now"] = round(time.time(), 1)
        state["interval"] = self.interval
        # Which miner this is, which the page shows: with a config file the
        # address was discovered rather than given, so it is worth being able
        # to see the one that was found.
        state["miner_api"] = "%s:%d" % (self.api.host, self.api.port)
        return state


class Handler(BaseHTTPRequestHandler):
    server_version = "avian-webui"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    poller = None
    verbose = False

    def do_GET(self):
        self._serve(body=True)

    def do_HEAD(self):
        self._serve(body=False)

    def _serve(self, body):
        path = self.path.split("?", 1)[0].rstrip("/") or "/"

        if path == "/":
            try:
                with open(PAGE, "rb") as fh:
                    page = fh.read()
            except OSError as exc:
                self._send(500, b"text/plain; charset=utf-8",
                           ("cannot read %s: %s\n" % (PAGE, exc)).encode(),
                           body)
                return
            self._send(200, "text/html; charset=utf-8", page, body)
        elif path == "/api/state":
            payload = json.dumps(self.poller.snapshot()).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", payload, body,
                       cache=False)
        else:
            self._send(404, "text/plain; charset=utf-8", b"not found\n", body)

    def _send(self, status, ctype, payload, body, cache=True):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        if not cache:
            self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(payload)

    def _reject(self):
        """Anything that is not a read. Allow is part of the contract, not
        politeness: it is how a client learns this server has no write side."""
        self.send_response(405)
        self.send_header("Allow", "GET, HEAD")
        self.send_header("Content-Length", "0")
        self.end_headers()

    do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = _reject

    def log_message(self, fmt, *args):
        if self.verbose:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def hostport(text, default_port):
    """host[:port], with IPv6 in brackets."""
    if text.startswith("["):
        host, _, rest = text[1:].partition("]")
        port = rest.lstrip(":")
    else:
        host, _, port = text.rpartition(":")
        if not host:               # no colon at all: it was a bare host
            host, port = text, ""
    if not port:
        return host, default_port
    try:
        return host, int(port)
    except ValueError:
        raise argparse.ArgumentTypeError("not a port: %r" % port)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="A read-only web dashboard for avian-miner.")
    ap.add_argument("--bind", default="0.0.0.0:4049", metavar="ADDR[:PORT]",
                    help="where to serve the dashboard. The default listens "
                         "on every interface so another machine can open it; "
                         "use 127.0.0.1:4049 to keep it to this one. "
                         "(default: %(default)s)")
    ap.add_argument("--miner", default="127.0.0.1:4048", metavar="ADDR[:PORT]",
                    help="the miner's own API, which it should be told to "
                         "open with --api-bind (default: %(default)s)")
    ap.add_argument("--miner-config", metavar="PATH",
                    help="the miner's own config file, the one passed to its "
                         "-c. The api-bind in it decides the address to poll, "
                         "so the two cannot disagree about the port, and it "
                         "is re-read when the file changes, so a rig whose "
                         "API moves does not need this restarted. Takes "
                         "precedence over --miner. On HiveOS the file is "
                         "/hive/miners/custom/avian-miner/avian-miner.conf")
    ap.add_argument("--interval", type=float, default=5.0, metavar="SECONDS",
                    help="seconds between polls (default: %(default)s)")
    ap.add_argument("--history", type=int, default=720, metavar="SAMPLES",
                    help="hashrate samples held in memory, per card and in "
                         "total; at the default interval 720 is an hour "
                         "(default: %(default)s)")
    ap.add_argument("--timeout", type=float, default=5.0, metavar="SECONDS",
                    help="miner API socket timeout (default: %(default)s)")
    ap.add_argument("--verbose", action="store_true",
                    help="log every request")
    args = ap.parse_args(argv)

    if args.interval <= 0:
        ap.error("--interval must be positive")
    if args.history < 2:
        ap.error("--history must be at least 2")

    bind_host, bind_port = hostport(args.bind, 4049)
    miner_host, miner_port = hostport(args.miner, 4048)

    config = MinerConfig(args.miner_config) if args.miner_config else None
    poller = Poller(MinerAPI(miner_host, miner_port, args.timeout),
                    args.interval, args.history, config)
    poller.start()

    Handler.poller = poller
    Handler.verbose = args.verbose

    server = ThreadingHTTPServer((bind_host, bind_port), Handler)
    server.daemon_threads = True

    shown = bind_host if bind_host not in ("0.0.0.0", "::") else "<this host>"
    reading = ("the api-bind in %s" % args.miner_config if config
               else "%s:%d" % (miner_host, miner_port))
    sys.stderr.write(
        "avian-webui: reading %s, serving http://%s:%d/ (read only)\n"
        % (reading, shown, bind_port))
    if bind_host in ("0.0.0.0", "::"):
        sys.stderr.write(
            "avian-webui: reachable from the network and unauthenticated; "
            "keep it off untrusted ones\n")
    sys.stderr.flush()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        sys.stderr.write("\navian-webui: stopping\n")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
