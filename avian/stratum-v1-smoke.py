#!/usr/bin/env python3
"""Stratum v1 still works.

The Stratum v2 backend is meant to be a third source of work, never a
replacement, so this serves a minimal Stratum v1 pool on loopback and asserts
the miner walks the whole v1 path against it: subscribe, authorize, take a
job, hash it, and submit a share.

The job's fields are well-formed but arbitrary, and the difficulty is set low
enough that almost any hash passes. That is deliberate: what is under test is
the miner's v1 plumbing, not whether this script can build a real Avian
template. A share arriving at all is the assertion.

Usage: avian/stratum-v1-smoke.py [path-to-cpuminer]
ALGO=x16rt or minotaurx (default) picks the algorithm the miner hashes with.
"""

import json
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MINER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, os.pardir, "cpuminer")

JOB = {
    "job_id": "01",
    "prevhash": "00" * 32,
    "coinb1": "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff0f03",
    "coinb2": "ffffffff0100f2052a01000000232103" + "11" * 32 + "ac00000000",
    "merkle_branch": [],
    "version": "00000132",
    "nbits": "207fffff",
    "ntime": "6ade317d",
    "clean_jobs": True,
}

submitted = threading.Event()
state = {"subscribed": False, "authorized": False, "notified": False}


def serve(sock):
    conn, _ = sock.accept()
    conn.settimeout(60)
    buffered = b""
    pending_notify = True

    def send(obj):
        conn.sendall((json.dumps(obj) + "\n").encode())

    while not submitted.is_set():
        if state["authorized"] and pending_notify:
            # A tiny difficulty, so the first hash almost certainly passes and
            # the test does not depend on how fast this machine is.
            send({"id": None, "method": "mining.set_difficulty",
                  "params": [0.0000001]})
            send({"id": None, "method": "mining.notify", "params": [
                JOB["job_id"], JOB["prevhash"], JOB["coinb1"], JOB["coinb2"],
                JOB["merkle_branch"], JOB["version"], JOB["nbits"],
                JOB["ntime"], JOB["clean_jobs"]]})
            state["notified"] = True
            pending_notify = False

        try:
            chunk = conn.recv(8192)
        except socket.timeout:
            break
        if not chunk:
            break
        buffered += chunk
        while b"\n" in buffered:
            line, buffered = buffered.split(b"\n", 1)
            if not line.strip():
                continue
            try:
                message = json.loads(line)
            except ValueError:
                continue
            method = message.get("method")
            ident = message.get("id")
            if method == "mining.subscribe":
                state["subscribed"] = True
                send({"id": ident, "result": [
                    [["mining.notify", "smoke"]], "deadbeef", 4],
                    "error": None})
            elif method == "mining.authorize":
                state["authorized"] = True
                send({"id": ident, "result": True, "error": None})
            elif method == "mining.submit":
                send({"id": ident, "result": True, "error": None})
                submitted.set()
            else:
                # extranonce.subscribe, configure, suggest_difficulty and
                # anything else: say yes and move on.
                send({"id": ident, "result": True, "error": None})
    conn.close()


def main():
    if not os.access(MINER, os.X_OK):
        print("FAIL: %s is not executable" % MINER)
        return 1

    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.listen(1)
    print("== serving a minimal Stratum v1 pool on 127.0.0.1:%d" % port)

    thread = threading.Thread(target=serve, args=(sock,), daemon=True)
    thread.start()

    miner = subprocess.Popen(
        [MINER, "-a", os.environ.get("ALGO", "minotaurx"), "-t", "2", "-o",
         "stratum+tcp://127.0.0.1:%d" % port, "-u", "smoke", "-p", "x"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print("== started %s" % MINER)

    deadline = time.time() + 60
    while time.time() < deadline and not submitted.is_set():
        if miner.poll() is not None:
            break
        time.sleep(0.5)

    miner.terminate()
    try:
        output = miner.communicate(timeout=10)[0].decode(errors="replace")
    except subprocess.TimeoutExpired:
        miner.kill()
        output = miner.communicate()[0].decode(errors="replace")
    sock.close()

    print()
    print("== assertions")
    failures = 0
    for name, ok in (("the miner subscribed", state["subscribed"]),
                     ("the miner authorized", state["authorized"]),
                     ("the pool sent it a job", state["notified"]),
                     ("the miner submitted a share", submitted.is_set())):
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
        if not ok:
            failures += 1

    print()
    if failures:
        print("FAIL: Stratum v1 is broken")
        print(output[-3000:])
        return 1
    print("OK: Stratum v1 still subscribes, takes a job, hashes and submits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
