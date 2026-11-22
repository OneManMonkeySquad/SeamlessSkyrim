"""Writes build/se-port/ae2se-all.txt (AE id -> SE id, one pair a line) from the public tables.

Inputs in build/se-port (see docs/BUILD.md, "SE 1.5.97"): se-ids.csv (addrlib.py on
version-1-5-97-0.bin), and from github.com/alandtse/skyrim_vr_address_library: se_ae.csv,
se-ae-attempted-match.csv (saved as attempted.csv) and offsets-1-6-318-0.csv. A candidate, not a
fact: check each pair against the code (sepair.py) before using it.
"""
import csv, pathlib
DATA = pathlib.Path(__file__).resolve().parents[2] / "build" / "se-port"
ae318 = {int(r[1], 16) - 0x140000000: int(r[0]) for r in csv.reader(open(DATA / "offsets-1-6-318-0.csv")) if r[0] != "aeid"}
se_off2id = {int(r[1], 16): int(r[0]) for r in csv.reader(open(DATA / "se-ids.csv")) if r[0] != "id"}
m = {}
for r in csv.DictReader(open(DATA / "attempted.csv")):
    s = int(r["sse_addr"], 16) - 0x140000000; a = int(r["ae_addr"], 16) - 0x140000000
    if a in ae318 and s in se_off2id:
        m.setdefault(ae318[a], se_off2id[s])
for r in csv.DictReader(open(DATA / "se_ae.csv")):
    m.setdefault(int(r["aeid"]), int(r["sseid"]))
with open(DATA / "ae2se-all.txt", "w") as f:
    for k in sorted(m):
        f.write(f"{k} {m[k]}\n")
print(len(m), "pairs")
