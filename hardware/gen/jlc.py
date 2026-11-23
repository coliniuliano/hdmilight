"""Query JLCPCB's assembly parts library (the undocumented endpoint their own parts search uses), to check that a
part is in their library and in stock before the BOM names it.
python3 jlc.py "keyword" [n]      -> candidates, basic parts first
python3 jlc.py C25804 C1525 ...   -> exact LCSC numbers
Be decent about it: it is their server and there is no published rate limit."""
import json, sys, urllib.request
URL = "https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList"
def query(keyword, size=25):
    body = json.dumps({"keyword": keyword, "currentPage": 1, "pageSize": size}).encode()
    req = urllib.request.Request(URL, data=body, headers={"Content-Type": "application/json", "User-Agent": "hdmilight-parts-lookup"})
    try:
        d = json.load(urllib.request.urlopen(req, timeout=30))
    except Exception as e:
        return [{"err": str(e)}]
    lst = ((d.get("data") or {}).get("componentPageInfo") or {}).get("list") or []
    out = []
    for c in lst:
        prices = c.get("componentPrices") or []
        p1 = prices[0].get("productPrice") if prices else None
        out.append(dict(lcsc=c.get("componentCode"), mpn=c.get("componentModelEn"), brand=c.get("componentBrandEn"),
                        pkg=c.get("componentSpecificationEn"), stock=c.get("stockCount") or 0,
                        lib=c.get("componentLibraryType"), pref=c.get("preferredComponentFlag"), price=p1,
                        desc=c.get("describe") or ""))
    return out
def fmt(c):
    if "err" in c: return str(c)
    kind = "BASIC" if c["lib"] == "base" else ("pref " if c["pref"] else "ext  ")
    return f"{c['lcsc']:10} {kind} stock {c['stock']:>8} ${c['price']!s:<8} {str(c['mpn'])[:26]:26} {str(c['pkg'])[:16]:16} {c['desc'][:70]}"
if __name__ == "__main__":
    args = sys.argv[1:]
    if all(a[0] == "C" and a[1:].isdigit() for a in args):
        for a in args:
            hit = [c for c in query(a, 5) if c.get("lcsc") == a]
            print(fmt(hit[0]) if hit else f"{a:10} not in JLC library")
    else:
        n = int(args[1]) if len(args) > 1 else 6
        res = [c for c in query(args[0], 50) if "err" in c or c["stock"] > 0]
        res.sort(key=lambda c: (c.get("lib") != "base", not c.get("pref"), -c.get("stock", 0)))
        for c in res[:n]: print(fmt(c))
