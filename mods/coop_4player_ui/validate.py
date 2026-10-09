import re, sys, xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path
GUID = re.compile(r'[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{16}')

# Paths hang off the script's own folder, so moving this mod's directory cannot break them.
HERE = Path(__file__).resolve().parent
orig = str(HERE / "extracted" / "ui" / "frontend ui" / "mp_grand_campaign.twui.xml")
mod  = sys.argv[1] if len(sys.argv) > 1 else str(HERE / "build" / "ui" / "frontend ui" / "mp_grand_campaign.twui.xml")

for name, fn in (("ORIGINAL", orig), ("MODIFIED", mod)):
    txt = open(fn, encoding="utf-8").read()
    try:
        ET.fromstring(txt); ok = "PARSES OK (well-formed XML)"
    except Exception as e:
        ok = "XML PARSE ERROR: " + str(e)
    print(name + ":", ok)

lines = open(mod, encoding="utf-8").read().split("\n")
he = next(i for i, l in enumerate(lines) if l.strip() == "</hierarchy>")
hthis = []
for l in lines[:he]:
    m = re.search(r'<[A-Za-z_]\w* this="(' + GUID.pattern + r')"', l)
    if m: hthis.append(m.group(1).upper())
c = Counter(hthis)
dups = [g for g, n in c.items() if n > 1]
print("hierarchy nodes:", len(hthis), "| duplicate hierarchy GUIDs:", len(dups), dups[:5])

def subtree_guids(tag):
    orx = re.compile(r'^(\t+)<' + tag + r' this=')
    for i, l in enumerate(lines[:he]):
        m = orx.match(l)
        if m:
            close = m.group(1) + "</" + tag + ">"
            for j in range(i + 1, he):
                if lines[j] == close:
                    s = set()
                    for x in lines[i:j + 1]:
                        s |= {g.upper() for g in GUID.findall(x)}
                    return s
    return set()

for a, b in (("panel_player1", "panel_player3"), ("panel_player2", "panel_player4")):
    ga, gb = subtree_guids(a), subtree_guids(b)
    print(a + " vs " + b + ": sizes %d/%d, shared GUIDs = %d" % (len(ga), len(gb), len(ga & gb)))
