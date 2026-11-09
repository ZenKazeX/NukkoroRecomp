"""Builds a self-contained HTML page to hand-review the machine-drafted Hungarian lines that are most likely to need a human
touch (banter, nicknames, insults, laughs, slang). Each line shows the English, the draft in an editable box (live check of
the 42-character segment limit and of the game font's glyph set) and an export button that produces the JSON for
tools/hungarian/draft_overrides.json.
    python -I make_review.py <tr dir: missing.json, out_*.json, allowed_chars.txt> <out.html>
"""
import glob, html, json, os, re, sys

GROUPS = [
    ("Nicknames, teasing and insults", r"\b(slowpoke|slow ?poke|slow|faker|wimp|loser|chump|sucker|pal|buddy|dude|kiddo|kid|pipsqueak|brat|runt|pest|nuisance|fool|fools|foolish|idiot|moron|stupid|dummy|coward|chicken|weakling|lazy|snail|turtle|dumb|silly|cocky|show-?off|hotshot|champ|genius|rookie|pathetic|useless|hopeless|ugly|shrimp|fuzzball|spiky|blue ?boy|tin can|bucket of bolts|hedgehog)\b"),
    ("Laughs, cheers and sound words", r"\b(ha ?ha|hahaha|heh|hee ?hee|hoo ?hoo|yahoo|woo-?hoo|whoopee|yeah|yay|yippee|ahh+|aaah+|ooh+|whoa+|oof|ugh|argh+|geez|gee|oops|yikes|hmph|tch)\b"),
    ("Slang, idioms and attitude", r"\b(gonna|wanna|gotta|outta|gotcha|whatever|bring it|come and get|piece of cake|no sweat|easy-?peasy|cool|awesome|sweet|rad|totally|get a life|take that|eat my|hey,? you|ya|y'all|c'mon|come on|let's roll|showtime|my pleasure|you bet|no way|big deal|nice try|dream on)\b"),
]

def main(tr, dst):
    missing = json.load(open(os.path.join(tr, 'missing.json'), encoding='utf-8'))
    allowed = open(os.path.join(tr, 'allowed_chars.txt'), encoding='utf-8').read().strip()
    drafts = {}
    for f in sorted(glob.glob(os.path.join(tr, 'out_*.json'))):
        for d in json.load(open(f, encoding='utf-8')):
            drafts[(d['section'], d['position'])] = d['hungarian']
    ov_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'draft_overrides.json')
    overrides = {}
    if os.path.exists(ov_path):
        overrides = {k: v for k, v in json.load(open(ov_path, encoding='utf-8')).items() if ':' in k}
    seen, groups = set(), []
    for title, pat in GROUPS:
        rx = re.compile(pat, re.I)
        items = []
        for m in missing:
            key = (m['section'], m['position'])
            if key in seen or not rx.search(m['english']): continue
            seen.add(key)
            k = '%s:%d' % key
            items.append({'key': k, 'en': m['english'], 'hu': overrides.get(k, drafts[key]), 'edited': k in overrides})
        groups.append((title, items))
    total = sum(len(i) for _, i in groups)
    out = ["<!doctype html><html lang='hu'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>",
           "<title>Hungarian lines to review</title><style>",
           ":root{--bg:#fff;--fg:#1a1a1a;--mut:#666;--line:#ddd;--ok:#2a7d3f;--bad:#b3261e;--card:#f6f6f4}",
           "@media(prefers-color-scheme:dark){:root{--bg:#1d1d1b;--fg:#eee;--mut:#aaa;--line:#444;--ok:#6fcf8a;--bad:#ff8a80;--card:#272725}}",
           "body{font:15px/1.5 system-ui,sans-serif;background:var(--bg);color:var(--fg);margin:0;padding:16px;max-width:900px;margin:auto}",
           "h1{font-size:20px;font-weight:500}h2{font-size:17px;font-weight:500;margin:28px 0 8px}",
           ".it{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 12px;margin:0 0 10px}",
           ".k{font:12px ui-monospace,monospace;color:var(--mut)}.en{margin:2px 0 6px;white-space:pre-wrap}",
           "textarea{width:100%;box-sizing:border-box;font:inherit;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:6px;min-height:56px}",
           ".st{font-size:12px;margin-top:2px}.ok{color:var(--ok)}.bad{color:var(--bad)}.ch textarea{border-color:var(--ok)}",
           "button{font:inherit;padding:6px 12px;border:1px solid var(--line);border-radius:6px;background:var(--card);color:var(--fg);cursor:pointer}",
           "#out{width:100%;min-height:140px;font:12px ui-monospace,monospace}.bar{position:sticky;top:0;background:var(--bg);padding:8px 0;border-bottom:1px solid var(--line);z-index:2}",
           "</style></head><body>",
           "<h1>Hungarian lines to review (%d)</h1>" % total,
           "<div class='bar'><button id='exp'>Export my edits as JSON</button> <span id='cnt' class='k'></span></div>",
           "<p class='k'>In the box, a line break is how the game breaks the line (each line at most 42 characters). Edit the draft; edited boxes get a green border. Export gives the text to paste into tools/hungarian/draft_overrides.json (or just send it to me).</p>"]
    for title, items in groups:
        out.append("<h2>%s (%d)</h2>" % (html.escape(title), len(items)))
        for it in items:
            out.append("<div class='it' data-k='%s' data-orig='%s'><div class='k'>%s%s</div><div class='en'>%s</div><textarea spellcheck='false'>%s</textarea><div class='st'></div></div>" % (
                it['key'], html.escape(it['hu'].replace(' / ', '\n'), quote=True), it['key'], ' (hand-set)' if it['edited'] else '',
                html.escape(it['en'].replace(' / ', '\n')), html.escape(it['hu'].replace(' / ', '\n'))))
    out.append("<h2>Export</h2><textarea id='out' readonly placeholder='Edits appear here'></textarea>")
    out.append("<script>var ALLOWED=new Set(%s);" % json.dumps(allowed + ' \n'))
    out.append("""function chk(box){var t=box.value,st=box.parentNode.querySelector('.st'),probs=[];
t.split('\\n').forEach(function(l,i){if(l.length>42)probs.push('line '+(i+1)+' is '+l.length+' characters (max 42)');});
var bad=[];for(var c of t){if(!ALLOWED.has(c)&&bad.indexOf(c)<0)bad.push(c);}
if(bad.length)probs.push('no glyph for: '+bad.join(' '));
st.className='st '+(probs.length?'bad':'ok');st.textContent=probs.length?probs.join('; '):'fits';
var it=box.parentNode;it.classList.toggle('ch',box.value!==it.dataset.orig);}
document.querySelectorAll('textarea:not(#out)').forEach(function(b){chk(b);b.addEventListener('input',function(){chk(b);count();});});
function count(){var n=document.querySelectorAll('.it.ch').length;document.getElementById('cnt').textContent=n+' edited';}
document.getElementById('exp').onclick=function(){var o={};document.querySelectorAll('.it.ch').forEach(function(it){o[it.dataset.k]=it.querySelector('textarea').value.replace(/\\n/g,' / ');});
var s=JSON.stringify(o,null,1);var out=document.getElementById('out');out.value=s;out.select();try{navigator.clipboard.writeText(s);}catch(e){}};
count();</script></body></html>""")
    open(dst, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print(total, 'lines;', ', '.join('%s %d' % (t, len(i)) for t, i in groups))

main(sys.argv[1], sys.argv[2])
