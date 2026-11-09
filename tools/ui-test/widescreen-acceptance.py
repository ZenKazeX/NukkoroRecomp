"""Acceptance checks for the widescreen 2D layout, measured from --hud-report.

Every check is a statement about the 640x480 virtual canvas, so it holds at
every resolution that shares an aspect ratio.
"""
import csv, glob, os, sys

CANVAS_W = 640.0
NATIVE = 4.0 / 3.0

ASPECTS = {
    'a169': 16 / 9, 'a1610': 16 / 10, 'a219': 21 / 9, 'a329': 32 / 9,
    'acustom': 2.37, 'aodd': 1100 / 823,
}


def anchor_x(anchor):
    return {'left': 0.0, 'center': CANVAS_W / 2, 'right': CANVAS_W}.get(
        anchor.split('-')[-1] if anchor.startswith(('top-', 'bottom-')) else anchor)


def check(path):
    name = os.path.basename(path)[:-4]
    key = name.split('_')[0]
    if key not in ASPECTS:
        return []
    s = NATIVE / max(ASPECTS[key], NATIVE)
    rows = list(csv.DictReader(open(path)))
    if not rows:
        return ['%s: no orthographic primitives captured' % name]

    frame = max(int(r['frame']) for r in rows) - 1
    sel = [r for r in rows if int(r['frame']) == frame]
    problems = []

    # 1. Every anchored primitive obeys x' = A + s * (x - A) exactly.
    for r in sel:
        a = anchor_x(r['anchor'])
        if a is None:      # fill / canvas keep their geometry
            if r['anchor'] == 'fill' and abs(float(r['mapped_x0']) - float(r['x0'])) > 0.01:
                problems.append('%s: fill primitive %s was moved' % (name, r['primitive']))
            continue
        for src, dst in (('x0', 'mapped_x0'), ('x1', 'mapped_x1')):
            want = a + s * (float(r[src]) - a)
            if abs(float(r[dst]) - want) > 0.05:
                problems.append('%s: prim %s %s -> %.3f, expected %.3f' %
                                (name, r['primitive'], src, float(r[dst]), want))

    # 2. The timer is the centre-anchored text run on the top row. Its mapped
    #    centre must be the exact centre of the canvas, offset only by the
    #    game's own half-pixel.
    timer = [r for r in sel if r['group'] == 'text' and r['anchor'] == 'center'
             and float(r['y0']) < 60.0]
    if not timer:
        problems.append('%s: no centred timer glyph run found' % name)
    else:
        lo = min(float(r['mapped_x0']) for r in timer)
        hi = max(float(r['mapped_x1']) for r in timer)
        src_lo = min(float(r['x0']) for r in timer)
        src_hi = max(float(r['x1']) for r in timer)
        centre = (lo + hi) / 2
        src_centre = (src_lo + src_hi) / 2
        want = CANVAS_W / 2 + s * (src_centre - CANVAS_W / 2)
        if abs(centre - want) > 0.05:
            problems.append('%s: timer centre %.3f, expected %.3f' % (name, centre, want))
        if abs(src_centre - CANVAS_W / 2) > 1.0:
            problems.append('%s: timer is not centred in the guest canvas (%.3f)' %
                            (name, src_centre))

    # 3. Hero and Dark gauges: the physical margin from each screen edge must be
    #    the original margin scaled by s, so the pair stays as symmetric as the
    #    original design made it.
    left = [r for r in sel if r['group'] == 'csd' and r['anchor'].endswith('left')
            and float(r['y0']) < 100.0 and float(r['x1']) > 0.0]
    right = [r for r in sel if r['group'] == 'csd' and r['anchor'].endswith('right')
             and float(r['y0']) < 100.0 and float(r['x0']) < CANVAS_W]
    if left and right:
        lm_src = min(float(r['x0']) for r in left)
        lm = min(float(r['mapped_x0']) for r in left)
        rm_src = CANVAS_W - max(float(r['x1']) for r in right)
        rm = CANVAS_W - max(float(r['mapped_x1']) for r in right)
        if abs(lm - lm_src * s) > 0.05:
            problems.append('%s: hero gauge margin %.3f, expected %.3f' % (name, lm, lm_src * s))
        if abs(rm - rm_src * s) > 0.05:
            problems.append('%s: dark gauge margin %.3f, expected %.3f' % (name, rm, rm_src * s))
        # symmetry: the original asymmetry must be preserved, not amplified
        if abs((lm - rm) - (lm_src - rm_src) * s) > 0.05:
            problems.append('%s: gauge asymmetry changed (%.3f vs %.3f)' %
                            (name, lm - rm, (lm_src - rm_src) * s))
    else:
        problems.append('%s: hero/dark gauge groups not both found' % name)

    # 4. Nothing anchored may be pushed outside the canvas that started inside.
    for r in sel:
        if float(r['x0']) >= 0.0 and float(r['x1']) <= CANVAS_W:
            if float(r['mapped_x0']) < -0.5 or float(r['mapped_x1']) > CANVAS_W + 0.5:
                problems.append('%s: prim %s left the canvas' % (name, r['primitive']))
    return problems


if __name__ == '__main__':
    root = sys.argv[1]
    total = 0
    for path in sorted(glob.glob(os.path.join(root, '*.csv'))):
        issues = check(path)
        label = os.path.basename(path)[:-4]
        if issues:
            total += len(issues)
            for i in issues[:6]:
                print('  FAIL ' + i)
            if len(issues) > 6:
                print('  ... %d more for %s' % (len(issues) - 6, label))
        else:
            print('  PASS ' + label)
    print('\n%d problems' % total)
