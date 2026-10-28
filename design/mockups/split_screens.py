import re, sys, pathlib
src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
html = src.read_text(encoding="utf-8")
style = re.search(r"<style>.*?</style>", html, re.S).group(0)
items = html.split('<div class="item">')[1:]
names = []
for it in items:
    dev, rest = it.split("<h3>", 1)
    title = re.sub(r"<.*?>", "", rest.split("</h3>")[0])
    slug = re.sub(r"[^a-z0-9]+", "-", title.split("·", 1)[1].split("(")[0].strip().lower()).strip("-")
    n = f"{len(names)+1:02d}-{slug}"
    page = ("<!doctype html><html><head><meta charset='utf-8'>" + style +
            "<style>html,body{margin:0;background:#000}.dev{transform:none;margin:0;outline:none;border-radius:0}.arrow{display:none}</style>"
            "</head><body>" + dev.rsplit("</div>", 0)[0] + "</body></html>")
    (out / f"{n}.html").write_text(page, encoding="utf-8")
    names.append(n)
print("\n".join(names))
