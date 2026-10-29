#!/usr/bin/env python3
"""Control and read back the downlink scheduler policy of the gNB.

Talks to the gNB's control endpoint (UDP, default 127.0.0.1:9099; the gNB runs
on the host network, so run this on the cell node).

  sched-ctl.py ping
  sched-ctl.py status
  sched-ctl.py policy pf|rr|wrr|wshare
  sched-ctl.py weight <rnti|*> <1..1000000>
  sched-ctl.py cap <rnti> <0|5..275>
  sched-ctl.py reset
  sched-ctl.py measure [--seconds N]
  sched-ctl.py map --containers NAME[,NAME...] [--target IP]

Every state change is read back with a second request and the command fails
unless the gNB reports the requested value.  `measure` reports the resource
blocks the MAC actually scheduled per UE over a window, which is the evidence
for any allocation claim.
"""
import argparse
import socket
import subprocess
import sys
import time


class CtlError(Exception):
    pass


def request(cmd, addr="127.0.0.1", port=9099, timeout=3.0):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        try:
            s.sendto(cmd.encode(), (addr, port))
            data, _ = s.recvfrom(65536)
        except socket.timeout:
            raise CtlError(f"no reply from {addr}:{port} (is the gNB running the controllable scheduler?)")
        except OSError as exc:
            raise CtlError(f"cannot reach {addr}:{port}: {exc}")
    text = data.decode(errors="replace")
    if not text.startswith("OK"):
        raise CtlError(text.strip() or "empty reply")
    return text


def _kv(tokens):
    out = {}
    for tok in tokens:
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def parse_status(text):
    lines = text.strip().splitlines()
    header = _kv(lines[0].split())
    ues = []
    header["slots"] = []
    for line in lines[1:]:
        if line.startswith("SLOT "):
            parts = line.split()
            kv = _kv(parts)
            header["slots"].append((int(parts[1]), int(kv["calls"]), int(kv["fail"])))
        if line.startswith("UE "):
            ue = _kv(line.split())
            ue["rnti"] = int(ue["rnti"], 16)
            for key in ("weight", "explicit", "cap", "granted_rbs", "granted_slots", "commit_fail", "mac_dl_rbs", "mac_dl_rbs_retx", "mac_dl_sdu_bytes"):
                ue[key] = int(ue[key])
            ues.append(ue)
    return header, ues


def get_status(**kw):
    return parse_status(request("GET", **kw))


def print_status(header, ues, out=sys.stdout):
    print(f"version={header['version']} policy={header['policy']} default_weight={header['default_weight']} "
          f"min_rb={header['min_rb']} scheduler_calls={header['calls']}", file=out)
    if int(header.get("ws_groups", 0)) or int(header.get("commit_fail", 0)):
        print(f"diagnostics: commit_fail={header['commit_fail']} (cce {header['fail_cce']}, pucch {header['fail_pucch']}) "
              f"multi_ue_slots={header['multi_ue_slots']} "
              f"wshare groups={header['ws_groups']} free_rb={header['ws_free_rb']} demand_rb={header['ws_demand_rb']} "
              f"planned_rb={header['ws_planned_rb']} committed_rb={header['ws_committed_rb']}", file=out)
        print(f"wshare slots: shared {header['ws_shared_slots']}, one UE at a time {header['ws_single_slots']}, "
              f"all grants refused {header['ws_lost_slots']}; sharing now in {int(header['ws_share_milli']) / 10:.0f}% of the contested slots", file=out)
    if any(f for _, _, f in header["slots"]):
        print("grants by slot index in the frame (calls/refused): "
              + "  ".join(f"{i}:{c}/{f}" for i, c, f in header["slots"]), file=out)
    print(f"{'rnti':<8}{'weight':>8}{'set':>5}{'cap':>6}{'granted_rbs':>14}{'mac_dl_rbs':>12}{'retx_rbs':>10}{'dl_sdu_bytes':>14}", file=out)
    for u in ues:
        print(f"0x{u['rnti']:04x}  {u['weight']:>8}{u['explicit']:>5}{u['cap']:>6}{u['granted_rbs']:>14}"
              f"{u['mac_dl_rbs']:>12}{u['mac_dl_rbs_retx']:>10}{u['mac_dl_sdu_bytes']:>14}", file=out)


def parse_rnti(text):
    try:
        value = int(text, 16)
    except ValueError:
        raise CtlError(f"bad RNTI {text!r}")
    return value


def set_and_verify(cmd, check, **kw):
    reply = request(cmd, **kw)
    sent_version = int(_kv(reply.split())["version"])
    header, ues = get_status(**kw)
    if int(header["version"]) < sent_version:
        raise CtlError(f"read-back version {header['version']} is older than the acknowledged {sent_version}")
    if not check(header, ues):
        raise CtlError(f"read-back does not show the requested state after '{cmd}'")
    return header, ues


def cmd_measure(seconds, **kw):
    h0, u0 = get_status(**kw)
    time.sleep(seconds)
    h1, u1 = get_status(**kw)
    before = {u["rnti"]: u for u in u0}
    rows = []
    for u in u1:
        b = before.get(u["rnti"], {"mac_dl_rbs": 0, "mac_dl_sdu_bytes": 0, "mac_dl_rbs_retx": 0})
        rows.append((u["rnti"], (u["mac_dl_rbs"] - b["mac_dl_rbs"]) % 2**32, (u["mac_dl_rbs_retx"] - b["mac_dl_rbs_retx"]) % 2**32,
                     u["mac_dl_sdu_bytes"] - b["mac_dl_sdu_bytes"]))
    total = sum(r[1] for r in rows)
    print(f"window={seconds}s policy={h1['policy']} version {h0['version']}->{h1['version']}"
          + ("  (policy or weights changed during the window)" if h0["version"] != h1["version"] else ""))
    print(f"{'rnti':<8}{'rbs':>12}{'share%':>9}{'retx_rbs':>10}{'sdu_bytes':>12}{'Mbit/s':>9}")
    for rnti, rbs, retx, byt in rows:
        share = 100.0 * rbs / total if total else 0.0
        print(f"0x{rnti:04x}  {rbs:>12}{share:>9.1f}{retx:>10}{byt:>12}{byt * 8 / seconds / 1e6:>9.2f}")
    agg = sum(r[3] for r in rows) * 8 / seconds / 1e6
    calls = int(h1["calls"]) - int(h0["calls"])
    if h1["version"] == h0["version"] and int(h1.get("ws_groups", 0)) > int(h0.get("ws_groups", 0)):
        d = {k: int(h1[k]) - int(h0[k]) for k in ("commit_fail", "multi_ue_slots", "ws_groups", "ws_free_rb", "ws_demand_rb", "ws_planned_rb", "ws_committed_rb")}
        g = d["ws_groups"]
        print(f"wshare per call: free {d['ws_free_rb'] / g:.1f} demand {d['ws_demand_rb'] / g:.1f} planned {d['ws_planned_rb'] / g:.1f} "
              f"committed {d['ws_committed_rb'] / g:.1f} RBs; commit_fail {d['commit_fail']}, multi-UE calls {d['multi_ue_slots']} of {g}")
        mode = {k: int(h1[k]) - int(h0[k]) for k in ("ws_shared_slots", "ws_single_slots", "ws_lost_slots")}
        print(f"wshare slots in window: shared {mode['ws_shared_slots']}, one UE at a time {mode['ws_single_slots']}, "
              f"all grants refused {mode['ws_lost_slots']}; sharing now in {int(h1['ws_share_milli']) / 10:.0f}% of the contested slots")
        old = {i: (c, f) for i, c, f in h0["slots"]}
        cells = []
        for i, c, f in h1["slots"]:
            c0, f0 = old.get(i, (0, 0))
            if c - c0 > 0:
                cells.append(f"{i}:{c - c0}/{f - f0}")
        if cells:
            print("per slot index in window (calls/refused grants): " + "  ".join(cells))
    elif h1["version"] == h0["version"] and int(h1["commit_fail"]) > int(h0["commit_fail"]):
        print(f"commit_fail {int(h1['commit_fail']) - int(h0['commit_fail'])}, multi-UE calls {int(h1['multi_ue_slots']) - int(h0['multi_ue_slots'])}")
    if calls > 0 and h1["version"] == h0["version"]:
        print(f"total: {agg:.2f} Mbit/s, {total / seconds:.0f} RBs/s, scheduler calls {calls / seconds:.0f}/s, {total / calls:.1f} RBs per call")
    else:
        print(f"total: {agg:.2f} Mbit/s, {total / seconds:.0f} RBs/s")
    return rows


def cmd_map(containers, target, **kw):
    """Match UE containers to RNTIs: ping the data network from each UE and see
    whose downlink byte counter at the gNB moves."""
    result = {}
    for name in containers:
        _, before = get_status(**kw)
        proc = subprocess.run(["docker", "exec", name, "ping", "-c", "6", "-i", "0.3", "-W", "1", "-I", "oaitun_ue1", target],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise CtlError(f"ping from {name} failed: {proc.stdout.strip()[-200:]} {proc.stderr.strip()[-200:]}")
        time.sleep(0.5)
        _, after = get_status(**kw)
        b = {u["rnti"]: u["mac_dl_sdu_bytes"] for u in before}
        deltas = sorted(((u["mac_dl_sdu_bytes"] - b.get(u["rnti"], 0), u["rnti"]) for u in after), reverse=True)
        if not deltas or deltas[0][0] <= 0 or (len(deltas) > 1 and deltas[0][0] < 3 * max(deltas[1][0], 1)):
            raise CtlError(f"cannot tell which RNTI belongs to {name}: downlink byte deltas {[(hex(r), d) for d, r in deltas]}")
        result[name] = deltas[0][1]
    if len(set(result.values())) != len(result):
        raise CtlError(f"two containers matched the same RNTI: {result}")
    for name, rnti in result.items():
        print(f"{name}\t0x{rnti:04x}")
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--addr", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9099)
    ap.add_argument("--timeout", type=float, default=3.0)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping")
    sub.add_parser("status")
    p = sub.add_parser("policy"); p.add_argument("name", choices=["pf", "rr", "wrr", "wshare"])
    p = sub.add_parser("weight"); p.add_argument("rnti"); p.add_argument("value", type=int)
    p = sub.add_parser("cap"); p.add_argument("rnti"); p.add_argument("rbs", type=int)
    sub.add_parser("reset")
    p = sub.add_parser("measure"); p.add_argument("--seconds", type=float, default=10.0)
    p = sub.add_parser("map"); p.add_argument("--containers", required=True); p.add_argument("--target", default="192.168.72.135")
    a = ap.parse_args(argv)
    kw = {"addr": a.addr, "port": a.port, "timeout": a.timeout}

    try:
        if a.cmd == "ping":
            print(request("PING", **kw).strip())
        elif a.cmd == "status":
            print_status(*get_status(**kw))
        elif a.cmd == "policy":
            h, u = set_and_verify(f"POLICY {a.name}", lambda h, u: h["policy"] == a.name, **kw)
            print_status(h, u)
        elif a.cmd == "weight":
            if a.rnti == "*":
                h, u = set_and_verify(f"WEIGHT * {a.value}", lambda h, u: int(h["default_weight"]) == a.value, **kw)
            else:
                r = parse_rnti(a.rnti)
                h, u = set_and_verify(f"WEIGHT {r:x} {a.value}",
                                      lambda h, u: any(x["rnti"] == r and x["weight"] == a.value and x["explicit"] == 1 for x in u), **kw)
            print_status(h, u)
        elif a.cmd == "cap":
            r = parse_rnti(a.rnti)
            h, u = set_and_verify(f"CAP {r:x} {a.rbs}", lambda h, u: any(x["rnti"] == r and x["cap"] == a.rbs for x in u), **kw)
            print_status(h, u)
        elif a.cmd == "reset":
            h, u = set_and_verify("RESET", lambda h, u: h["policy"] == "pf" and not any(x["explicit"] or x["cap"] for x in u), **kw)
            print_status(h, u)
        elif a.cmd == "measure":
            cmd_measure(a.seconds, **kw)
        elif a.cmd == "map":
            cmd_map([c for c in a.containers.split(",") if c], a.target, **kw)
    except CtlError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
