#!/usr/bin/env python3
# 2016 client — key admin. Mints / revokes / lists keys in keys.json (the server reads it live).
#
#   python admin.py create weekly            # -> 2016-XXXX-XXXX-XXXX
#   python admin.py create monthly 5         # mint 5 monthly keys
#   python admin.py create lifetime
#   python admin.py delete 2016-XXXX-XXXX-XXXX
#   python admin.py list
import json, os, sys, time, secrets

HERE = os.path.dirname(os.path.abspath(__file__))
DB   = os.path.join(HERE, "keys.json")
TIERS = {"weekly": 0, "monthly": 1, "lifetime": 2}
TIERNAME = {0: "weekly", 1: "monthly", 2: "lifetime"}
ALPHABET = "ACDEFGHJKLMNPQRSTUVWXYZ2345679"   # no ambiguous chars

def load():
    try:  return json.load(open(DB))
    except Exception: return {}
def save(d): json.dump(d, open(DB, "w"), indent=2)

def gen_key():
    g = lambda: "".join(secrets.choice(ALPHABET) for _ in range(4))
    return f"2016-{g()}-{g()}-{g()}"

def cmd_create(tier, n):
    if tier not in TIERS: print("tier must be weekly/monthly/lifetime"); return
    d = load(); out = []
    for _ in range(n):
        k = gen_key()
        while k in d: k = gen_key()
        d[k] = {"tier": TIERS[tier], "hwid": None, "activated": None, "revoked": False, "created": int(time.time())}
        out.append(k)
    save(d)
    for k in out: print(k)

def cmd_delete(key):
    d = load(); key = key.strip().upper()
    if key in d: del d[key]; save(d); print("deleted", key)
    else: print("not found", key)

def cmd_list():
    d = load()
    if not d: print("(no keys)"); return
    for k, e in d.items():
        bound = e.get("hwid") or "-"
        state = "REVOKED" if e.get("revoked") else "bound" if e.get("hwid") else "unused"
        print(f"{k}  {TIERNAME.get(e['tier'],'?'):8}  {state:8}  hwid={bound}")

if __name__ == "__main__":
    a = sys.argv[1:]
    if not a: print(__doc__); sys.exit()
    if a[0] == "create":  cmd_create(a[1] if len(a) > 1 else "weekly", int(a[2]) if len(a) > 2 else 1)
    elif a[0] == "delete" and len(a) > 1: cmd_delete(a[1])
    elif a[0] == "list":  cmd_list()
    else: print(__doc__)
