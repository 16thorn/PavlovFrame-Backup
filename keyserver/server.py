#!/usr/bin/env python3
# 2016 client — key validation server. Plain-HTTP (personal use). Stores keys in keys.json; the client
# hits /validate to activate + bind a key to its HWID. Admin (admin.py) mints/revokes keys in keys.json.
#
#   pip install flask
#   python server.py            # serves on 0.0.0.0:8016
#
# Client points at this host via files/keyserver.txt on the headset (e.g. "192.168.1.20:8016").
import json, os, time, threading
from flask import Flask, request

HERE = os.path.dirname(os.path.abspath(__file__))
DB   = os.path.join(HERE, "keys.json")
LOCK = threading.Lock()
DAYS = {0: 7, 1: 30, 2: 0}   # tier -> duration days (0 = lifetime)

def load():
    with LOCK:
        try:  return json.load(open(DB))
        except Exception: return {}
def save(d):
    with LOCK:
        json.dump(d, open(DB, "w"), indent=2)

app = Flask(__name__)

@app.get("/validate")
def validate():
    key = (request.args.get("key") or "").strip().upper()
    hwid = (request.args.get("hwid") or "").strip()
    d = load()
    e = d.get(key)
    if not e:                          return "FAIL nokey"
    if e.get("revoked"):               return "FAIL revoked"
    # bind on first use (1 key = 1 device)
    if not e.get("hwid"):
        e["hwid"] = hwid; e["activated"] = int(time.time()); save(d)
    if e["hwid"] != hwid:              return "FAIL boundelsewhere"
    tier = int(e["tier"]); days = DAYS.get(tier, 0)
    if days > 0:
        expiry = int(e["activated"]) + days * 86400
        if time.time() >= expiry:      return "FAIL expired"
    else:
        expiry = 4102444800            # ~year 2100 = lifetime
    return f"OK {tier} {expiry}"

@app.get("/list")
def _list():   # quick read-only peek (admin.py is the real tool)
    return {"count": len(load())}

if __name__ == "__main__":
    if not os.path.exists(DB): save({})
    app.run(host="0.0.0.0", port=8016)
