"""Generates panel.svg.fragment (the screen's artwork) and panel.h (where
display.cpp places it), and sets definition.json's resources.display.

The screen: the Earth in space inside a dial from the moon (sub) to the sun
(up): a smooth sphere lit on the sun's side, three meridians turning with it
and its axis through the poles. Run from the repository root:
    python tools/make_panel.py
"""
import json
import math

PREFIX = 'earthshine'

# Geometry, in screen pixels (480 x 220, y down).
CX, CY = 240, 128          # the Earth's centre
DIAL_R = 80                # the dial's arc
A_MOON, A_SUN = 210, -30   # the arc's ends, degrees with y up: moon to sun clockwise over the top
R_NEAR, R_FAR = 63, 15     # the Earth's radius at Dry 100 and 0 (paths reach 64 px at most)
ICON_MOON, ICON_SUN = A_MOON + 13, A_SUN - 13
POINTER = (73, 89)         # the pointer's span from the centre

# Palette: the day side, the night disc (its stroke the limb), the pointer,
# the meridians and the axis.
MERIDIAN = '#3C3C3C'
PALETTE = [('#646464', '#646464', 0.5), ('#000000', '#323232', 0.5), ('#CDCDCD', '#CDCDCD', 0.25),
           (MERIDIAN, MERIDIAN, 0.5), ('#969696', '#969696', 0.25)]

# Nodes: the day side and the three meridians (the four mutable outlines),
# the header's eight bars, the night disc, the pointer and the axis.
NODE_DAY, NODE_MERIDIANS, MERIDIANS = 0, 1, 3
NODE_BARS, NODE_DISC, NODE_POINTER, NODE_AXIS = 4, 12, 13, 14
TEXT_SUB, TEXT_UP = 16, 17
POINTS = 64
AXIS_HALF = 34             # the axis's authored half length; display.cpp scales it

# Text widths at 10 px, Univers Next Light, for centring the interval labels.
WIDTH = {'-': 3.3, '+': 5.8, '1': 5.6, '2': 5.6, '4': 5.6, '7': 5.6, '9': 5.6,
         'r': 3.3, 'i': 2.2, 's': 4.6, 'e': 5.3, 'f': 2.9, 'a': 5.2, 'l': 2.2}
SUB = ['-12', '-24', '-7']
UP = ['+12', 'rise', 'fall']  # the octave, or the halo rising or falling


def num(x):
    s = ('%.4f' % x).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def stars():
    """Fixed positions and greys, seeded as the mockup's."""
    s = 7
    out = []

    def rnd():
        nonlocal s
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        return s / 4294967296

    while len(out) < 44:
        x, y = 6 + rnd() * 468, 44 + rnd() * 170
        if 205 < x < 275 and y > 182:
            continue
        g = '#969696' if rnd() < 0.2 else '#4B4B4B' if rnd() < 0.55 else '#323232'
        r = 0.9 if rnd() < 0.15 else 0.6
        out.append((x, y, g, r))
    return out


def polar(r, degrees):
    a = math.radians(degrees)
    return CX + r * math.cos(a), CY - r * math.sin(a)


def path(points, **attrs):
    d = 'M' + ' L'.join('%s,%s' % (num(x), num(y)) for x, y in points) + ' Z'
    extra = ''.join(' %s="%s"' % (k.replace('_', '-'), v) for k, v in attrs.items())
    return '<path d="%s"%s/>' % (d, extra)


def solid(points, colour, width=0.25, **attrs):
    return path(points, fill=colour, stroke=colour, stroke_width=width, **attrs)


def bar(r0, r1, degrees, width):
    """A thin closed bar from r0 to r1 along an angle."""
    a = math.radians(degrees)
    ux, uy = math.cos(a), -math.sin(a)
    nx, ny = -uy * width / 2, ux * width / 2
    x0, y0 = CX + r0 * ux, CY + r0 * uy
    x1, y1 = CX + r1 * ux, CY + r1 * uy
    return [(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)]


def crescent(cx, cy):
    """The moon: a disc of 5 px less one of 4.2 px nudged up and right, as one outline."""
    ox, oy, r_out = cx, cy, 5.0
    ix, iy, r_in = cx + 2.4, cy - 1.6, 4.2
    # The circles' crossings.
    d = math.hypot(ix - ox, iy - oy)
    a = (r_out ** 2 - r_in ** 2 + d ** 2) / (2 * d)
    h = math.sqrt(r_out ** 2 - a ** 2)
    mx, my = ox + a * (ix - ox) / d, oy + a * (iy - oy) / d
    p1 = (mx + h * (iy - oy) / d, my - h * (ix - ox) / d)
    p2 = (mx - h * (iy - oy) / d, my + h * (ix - ox) / d)
    ang = lambda c, p: math.atan2(p[1] - c[1], p[0] - c[0])
    a1, a2 = ang((ox, oy), p1), ang((ox, oy), p2)
    # The outer arc away from the inner disc, then the inner arc back.
    away = math.atan2(oy - iy, ox - ix)

    def arc(c, r, start, end, n, toward):
        span = (end - start) % (2 * math.pi)
        if not (abs(((toward - start) % (2 * math.pi))) < span):
            span -= 2 * math.pi
        return [(c[0] + r * math.cos(start + span * k / n), c[1] + r * math.sin(start + span * k / n)) for k in range(n + 1)]

    outer = arc((ox, oy), r_out, a1, a2, 24, away)
    b1, b2 = ang((ix, iy), p2), ang((ix, iy), p1)
    inner = arc((ix, iy), r_in, b1, b2, 16, away)[1:-1]
    return outer + inner


def main():
    nodes = NODE_AXIS + 1
    texts = 18
    out = ['<g id="%s_panel">' % PREFIX]
    out.append('<!-- SDK two-page header: keep this block before machine artwork below y=38. -->')
    out.append('<path d="M0,0 L480,0 L480,38 L0,38 Z" fill="#000000"/>')
    greys = ['#000000', '#323232', '#4B4B4B', '#646464', '#7D7D7D', '#969696', '#AFAFAF', '#CDCDCD']
    for i, g in enumerate(greys):
        out.append('<path id="%s_node_%d" transform="matrix(1 0 0 1 %d 0)" d="M0,0 L60,0 L60,20 L0,20 Z" fill="%s" stroke="%s" stroke-width="0.5"/>'
                   % (PREFIX, NODE_BARS + i, 60 * i, g, g))
    header = [(5, 10, '#CBCBCB', 10), (125, 10, '#CBCBCB', 10), (245, 10, '#000000', 10), (365, 10, '#000000', 10),
              (65, 17.4, '#CBCBCB', 20), (185, 17.4, '#CBCBCB', 20), (305, 17.4, '#000000', 20), (425, 17.4, '#000000', 20),
              (5, 33, '#5A5A5A', 10), (125, 33, '#5A5A5A', 10), (245, 33, '#5A5A5A', 10), (365, 33, '#5A5A5A', 10),
              (65, 33, '#707070', 13), (185, 33, '#707070', 13), (305, 33, '#707070', 13), (425, 33, '#707070', 13)]
    for i, (x, y, fill, size) in enumerate(header):
        out.append('<text id="%s_text_%d" transform="matrix(1 0 0 1 %s %s)" fill="%s" font-family="UniversNextPro-Light" font-size="%dpx">-</text>'
                   % (PREFIX, i, num(x), num(y), fill, size))

    # Space: stars, then the dial's arc and ticks, all fixed.
    for x, y, g, r in stars():
        out.append('<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="%s" stroke-width="0.1"/>' % (num(x), num(y), num(r), g, g))
    arc = [polar(DIAL_R - 0.25, A_MOON + (A_SUN - A_MOON) * k / 31) for k in range(32)]
    arc += [polar(DIAL_R + 0.25, A_SUN + (A_MOON - A_SUN) * k / 31) for k in range(32)]
    out.append(solid(arc, '#4B4B4B', 0.1))
    for k in range(25):
        a = A_MOON + (A_SUN - A_MOON) * k / 24
        major = k % 12 == 0
        out.append(solid(bar(DIAL_R, DIAL_R + (6 if major else 3), a, 0.75 if major else 0.5),
                         '#969696' if major else '#4B4B4B', 0.1))

    # The icons: the moon a crescent, the sun a disc and eight rays, in white.
    mx, my = polar(DIAL_R, ICON_MOON)
    sx, sy = polar(DIAL_R, ICON_SUN)
    out.append(solid(crescent(mx, my), '#CDCDCD', 0.1))
    out.append('<circle cx="%s" cy="%s" r="3.2" fill="#FFFFFF" stroke="#FFFFFF" stroke-width="0.1"/>' % (num(sx), num(sy)))
    for k in range(8):
        a = math.radians(45 * k)
        ux, uy = math.cos(a), math.sin(a)
        nx, ny = -uy * 0.375, ux * 0.375
        ray = [(sx + 5 * ux + nx, sy + 5 * uy + ny), (sx + 7.5 * ux + nx, sy + 7.5 * uy + ny),
               (sx + 7.5 * ux - nx, sy + 7.5 * uy - ny), (sx + 5 * ux - nx, sy + 5 * uy - ny)]
        out.append(solid(ray, '#FFFFFF', 0.1))

    # The Earth: the night disc, the day side over it, then the meridians and axis.
    place = 'transform="matrix(1 0 0 1 %d %d)"' % (CX, CY)
    out.append('<circle id="%s_node_%d" %s cx="0" cy="0" r="32" fill="#000000" stroke="#323232" stroke-width="0.5"/>'
               % (PREFIX, NODE_DISC, place))
    day = [(32 * math.cos(2 * math.pi * k / POINTS), 32 * math.sin(2 * math.pi * k / POINTS)) for k in range(POINTS)]
    out.append(path(day, id='%s_node_%d' % (PREFIX, NODE_DAY), transform='matrix(1 0 0 1 %d %d)' % (CX, CY),
                    fill='#646464', stroke='#646464', stroke_width=0.5))
    ring = [(30 * math.cos(2 * math.pi * k / POINTS), 30 * math.sin(2 * math.pi * k / POINTS)) for k in range(POINTS)]
    for m in range(MERIDIANS):
        out.append(path(ring, id='%s_node_%d' % (PREFIX, NODE_MERIDIANS + m), transform='matrix(1 0 0 1 %d %d)' % (CX, CY),
                        fill='none', stroke=MERIDIAN, stroke_width=0.5))
    # The axis, upright through the centre; display.cpp turns and scales it.
    out.append(path([(-0.3, -AXIS_HALF), (0.3, -AXIS_HALF), (0.3, AXIS_HALF), (-0.3, AXIS_HALF)],
                    id='%s_node_%d' % (PREFIX, NODE_AXIS), transform='matrix(1 0 0 1 %d %d)' % (CX, CY),
                    fill='#969696', stroke='#969696', stroke_width=0.25))
    # The pointer, along +x from the centre; display.cpp turns it.
    p0, p1 = POINTER
    out.append(path([(p0, -0.75), (p1, -0.75), (p1, 0.75), (p0, 0.75)], id='%s_node_%d' % (PREFIX, NODE_POINTER),
                    transform='matrix(1 0 0 1 %d %d)' % (CX, CY), fill='#CDCDCD', stroke='#CDCDCD', stroke_width=0.25))

    # The intervals under the icons.
    for slot, (x, y) in ((TEXT_SUB, (mx, my + 18)), (TEXT_UP, (sx, sy + 18))):
        out.append('<text id="%s_text_%d" transform="matrix(1 0 0 1 %s %s)" fill="#969696" font-family="UniversNextPro-Light" font-size="10px">-</text>'
                   % (PREFIX, slot, num(x - 8), num(y)))
    for i, (fill, stroke, width) in enumerate(PALETTE):
        out.append('<circle id="%s_palette_%d" cx="640" cy="0" r="1" fill="%s" stroke="%s" stroke-width="%s"/>'
                   % (PREFIX, i, fill, stroke, num(width)))
    out.append('</g>')
    open('panel.svg.fragment', 'w').write('\n'.join(out) + '\n')

    width = lambda s: int(round(sum(WIDTH[c] for c in s) * 1024))
    h = []
    w = h.append
    w('// Generated by tools/make_panel.py: where display.cpp places the artwork, in')
    w('// pixels unless named Q10, Q14 or tenths (of a degree).')
    w('#pragma once')
    w('#include <opxy/types.hpp>')
    w('')
    w('namespace earthshine { namespace panel {')
    w('const opxy::u32 kNodes = %d, kTexts = %d, kPalette = %d, kPoints = %d;' % (nodes, texts, len(PALETTE), POINTS))
    w('enum Node { kNodeDay = %d, kNodeMeridians = %d, kNodeBars = %d, kNodeDisc = %d, kNodePointer = %d, kNodeAxis = %d };'
      % (NODE_DAY, NODE_MERIDIANS, NODE_BARS, NODE_DISC, NODE_POINTER, NODE_AXIS))
    w('const opxy::u32 kMeridians = %d;' % MERIDIANS)
    w('const opxy::i32 kAxisHalf = %d;' % AXIS_HALF)
    w('enum Text { kTextSub = %d, kTextUp = %d };' % (TEXT_SUB, TEXT_UP))
    w('enum Shade { kShadeDay = 0, kShadeNight = 1, kShadePointer = 2, kShadeMeridian = 3, kShadeAxis = 4 };')
    w('const opxy::i32 kCx = %d, kCy = %d, kNear = %d, kFar = %d;' % (CX, CY, R_NEAR, R_FAR))
    w('// The dial from moon to sun, and the sun\'s direction from the centre, y up.')
    w('const opxy::i32 kMoonTenths = %d, kSunTenths = %d, kSunIconTenths = %d;' % (A_MOON * 10, A_SUN * 10, ICON_SUN * 10))
    w('// The intervals\' labels: their centres, and each choice\'s width, Q10.')
    w('const opxy::i32 kSubX = %d, kUpX = %d, kLabelY = %d;' % (round(mx * 1024), round(sx * 1024), round((my + 18) * 1024)))
    w('const char* const kSub[3] = {%s};' % ', '.join('"%s"' % s for s in SUB))
    w('const char* const kUp[3] = {%s};' % ', '.join('"%s"' % s for s in UP))
    w('const opxy::i32 kSubWidth[3] = {%s};' % ', '.join(str(width(s)) for s in SUB))
    w('const opxy::i32 kUpWidth[3] = {%s};' % ', '.join(str(width(s)) for s in UP))
    w('// Decay in tenths of a second and Tone in hertz across their controls, as the effect\'s.')
    w('const opxy::i32 kDecayTenths[33] = {%s};' % ', '.join(str(round(10 * 20 ** (i / 32) * 1024)) for i in range(33)))
    w('const opxy::i32 kToneHz[33] = {%s};' % ', '.join(str(round(700 * (9000 / 700) ** (i / 32))) for i in range(33)))
    w('}}')
    open('panel.h', 'w').write('\n'.join(h) + '\n')

    definition = json.load(open('definition.json'))
    definition['resources']['display'] = {'stateBytes': 60, 'stateAlignment': 4, 'paths': 1 + MERIDIANS, 'points': POINTS,
                                          'nodes': nodes, 'texts': texts, 'palette': len(PALETTE)}
    open('definition.json', 'w').write(json.dumps(definition, indent=2) + '\n')
    print('nodes %d, paths %d, texts %d, palette %d' % (nodes, 1 + MERIDIANS, texts, len(PALETTE)))


if __name__ == '__main__':
    main()
