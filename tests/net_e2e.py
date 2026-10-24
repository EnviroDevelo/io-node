#!/usr/bin/env python3
"""The I/O board on Ethernet end to end, on the PC: the supervisor (cems-platform build/netsim, real
sockets: CEMS_IOB_TCP=1) and this repository's virtual board (build/vnode_net: the real node core and
node_net.c). A new board is found and given board slot 1 through the supervisor's HTTPS API, comes
online over Modbus TCP, and its inputs and outputs work.
    make net-e2e            (builds both, then runs this)"""
import argparse, http.cookiejar, json, os, ssl, subprocess, sys, tempfile, time, urllib.parse, urllib.request
HERE = os.path.dirname(os.path.abspath(__file__))
MB, WEB, TCP, DISC = 15913, 18913, 15502, 15223
BASE = f"https://127.0.0.1:{WEB}"
CTX = ssl._create_unverified_context()
jar = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX), urllib.request.HTTPCookieProcessor(jar))
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond: passed += 1; print("  ok  ", what)
    else: failed += 1; print("  FAIL:", what)


def api(path, form=None):
    data = urllib.parse.urlencode(form).encode() if form is not None else None
    req = urllib.request.Request(BASE + path, data=data, headers={"X-CEMS": "1", "Connection": "close"})
    try:
        with op.open(req, timeout=10) as f:
            return f.status, json.loads(f.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def wait(pred, secs=5.0):
    t = time.time() + secs
    while time.time() < t:
        try:
            v = pred()
            if v: return v
        except OSError:
            pass
        time.sleep(0.1)
    return None


def board(n):
    return api("/api/iob")[1]["boards"][n - 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plat", default=os.path.join(HERE, "..", "..", "cems-platform"))
    a = ap.parse_args()
    tmp = tempfile.mkdtemp()
    vn = subprocess.Popen([os.path.join(HERE, "..", "build", "vnode_net"), "-t", str(TCP), "-d", str(DISC)],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    env = dict(os.environ, CEMS_IOB_TCP="1", CEMS_IOB_TCP_PORT=str(TCP), CEMS_IOB_DISC_PORT=str(DISC),
               CEMS_IOB_DISC_TO="127.0.0.1", CEMS_IOB_TCP_HOST="127.0.0.1")
    unit = subprocess.Popen([os.path.join(a.plat, "build", "netsim"), str(MB), str(WEB), os.path.join(tmp, "f.bin"),
                             os.path.join(tmp, "n.bin")], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, text=True, env=env)
    try:
        check(vn.stdout.readline().strip() == "ready", "virtual board up")
        check(wait(lambda: api("/api/info")[0] == 200), "supervisor up")
        check(api("/api/login", {"level": "admin", "password": "2222"})[0] == 200, "admin login")
        unit.stdin.write("set 110 1\n"); unit.stdin.flush()          # iob1_enable
        check(wait(lambda: board(1)["state"] == "not responding", 4), "board 1 not responding: the board is new")
        check(api("/api/iob/net/find", {})[0] == 202, "find asked")
        j = wait(lambda: (lambda r: r if not r["busy"] else None)(api("/api/iob/net")[1]), 3)
        check(j and j["ok"] and len(j["boards"]) == 1, "one board found over UDP")
        b = j["boards"][0] if j and j["boards"] else {}
        check(b.get("new") and b.get("serial") == "VNODE-NET" and b.get("mac", "").startswith("02:"),
              f"it is new, with its serial and MAC: {b}")
        check(api("/api/iob/net/set", {"uid": b.get("uid", ""), "board": 1})[0] == 202, "set asked")
        j = wait(lambda: (lambda r: r if not r["busy"] else None)(api("/api/iob/net")[1]), 3)
        check(j and j["ok"] and j["boards"][0]["ip"] == "192.168.1.61" and j["boards"][0]["slot"] == 1,
              "the board took board 1's IP and address")
        check("settings ip 192.168.1.61 address 1" in vn.stdout.readline(), "the board saved its settings")
        check(wait(lambda: board(1)["state"] == "online", 4), "board 1 online over Modbus TCP")
        vn.stdin.write("in 5\n"); vn.stdin.flush()
        check(wait(lambda: board(1)["inputs"] == 5, 3), "inputs 1 and 3 seen by the supervisor")
        e0 = board(1)["errors"]; time.sleep(2)
        check(board(1)["errors"] == e0, f"no failed request in 2 s of running ({board(1)['errors'] - e0})")
        # a second find sees it running and belonging to this supervisor
        api("/api/iob/net/find", {})
        j = wait(lambda: (lambda r: r if not r["busy"] else None)(api("/api/iob/net")[1]), 3)
        check(j and j["boards"][0]["controlled"] and j["boards"][0]["master_ip"] == "192.168.1.50", "running, under this supervisor")
        vn.kill(); vn.wait()
        check(wait(lambda: board(1)["state"] == "not responding", 4), "board gone: not responding")
    finally:
        unit.terminate(); unit.wait(10)
        if vn.poll() is None: vn.kill()
    print(f"NET E2E: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
