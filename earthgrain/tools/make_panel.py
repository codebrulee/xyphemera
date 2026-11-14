"""Generates panel.svg.fragment (the screen's artwork) and panel.h (where
display.cpp places it), and sets definition.json's resources.display.

The screen: Earthshine's globe (a night disc with its limb, a crescent of day)
among fixed stars, four outlines display.cpp reshapes each frame, and 480 short
dashes it places, turns and scales to draw each mode's lines. Run from the
machine's folder:
    python tools/make_panel.py
"""
import json
import math

PREFIX = 'earthgrain'

# Geometry, in screen pixels (480 x 220, y down).
CX, CY, R = 240, 128, 63
LEAN, TIP = 23.44, 18.0          # the axis's lean and tip toward us, degrees
SUN = -43.0                      # the sun's direction on the screen, degrees, y up
SHADOW = 0.2                     # the share of the globe that is day (a crescent toward the sun)
POINTS = 64

# Palette: twelve greys (lines and fills), then the night disc and its limb, then night
# with a white, middling and dim edge (Plates' lifted plate, flashing with its slice).
GREYS = [0x1D, 0x28, 0x32, 0x3C, 0x4B, 0x57, 0x64, 0x7D, 0x96, 0xAF, 0xCD, 0xFF]
NIGHT, EDGE, EDGE_MID, EDGE_DIM = len(GREYS), len(GREYS) + 1, len(GREYS) + 2, len(GREYS) + 3

# Nodes: the day side and three more outlines (the mutable ones), the header's
# eight bars, the night disc, the axis (kept, hidden), the dashes, then the dots (grains:
# Orbit's marks and Satellite's satellites).
NODE_DAY, NODE_MERIDIANS, MERIDIANS = 0, 1, 3
NODE_BARS = 4
NODE_DISC, NODE_AXIS = 12, 13
NODE_SPECKS = 14
SPECKS = 456
NODE_DOTS = NODE_SPECKS + SPECKS
DOTS = 24
AXIS_HALF = 34
STATE_BYTES = 2556  # sizeof(DisplayState): checked against the compiled type by sdk.py test


def num(x):
    s = ('%.4f' % x).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def stars():
    """Fixed positions and greys, seeded as Earthshine's."""
    s = 7
    out = []

    def rnd():
        nonlocal s
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        return s / 4294967296

    while len(out) < 44:
        x, y = 6 + rnd() * 468, 44 + rnd() * 170
        if 168 < x < 312 and 56 < y < 200:   # not behind the globe
            continue
        g = '#969696' if rnd() < 0.2 else '#4B4B4B' if rnd() < 0.55 else '#323232'
        r = 0.9 if rnd() < 0.15 else 0.6
        out.append((x, y, g, r))
    return out


def path(points, **attrs):
    d = 'M' + ' L'.join('%s,%s' % (num(x), num(y)) for x, y in points) + ' Z'
    extra = ''.join(' %s="%s"' % (k.replace('_', '-'), v) for k, v in attrs.items())
    return '<path d="%s"%s/>' % (d, extra)


def main():
    light = 1 - math.acos(max(-1, min(1, 2 * SHADOW - 1))) / math.pi
    sz = math.cos(math.pi * (1 - light))
    sun = (math.cos(math.radians(SUN)), math.sin(math.radians(SUN)))
    specks = SPECKS
    nodes = NODE_DOTS + DOTS
    texts = 16

    out = ['<g id="%s_panel">' % PREFIX]
    out.append('<!-- SDK two-page header: keep this block before machine artwork below y=38. -->')
    out.append('<path d="M0,0 L480,0 L480,38 L0,38 Z" fill="#000000"/>')
    bars = ['#000000', '#323232', '#4B4B4B', '#646464', '#7D7D7D', '#969696', '#AFAFAF', '#CDCDCD']
    for i, g in enumerate(bars):
        out.append('<path id="%s_node_%d" transform="matrix(1 0 0 1 %d 0)" d="M0,0 L60,0 L60,20 L0,20 Z" fill="%s" stroke="%s" stroke-width="0.5"/>'
                   % (PREFIX, NODE_BARS + i, 60 * i, g, g))
    header = [(5, 10, '#CBCBCB', 10), (125, 10, '#CBCBCB', 10), (245, 10, '#000000', 10), (365, 10, '#000000', 10),
              (65, 17.4, '#CBCBCB', 20), (185, 17.4, '#CBCBCB', 20), (305, 17.4, '#000000', 20), (425, 17.4, '#000000', 20),
              (5, 33, '#5A5A5A', 10), (125, 33, '#5A5A5A', 10), (245, 33, '#5A5A5A', 10), (365, 33, '#5A5A5A', 10),
              (65, 33, '#707070', 13), (185, 33, '#707070', 13), (305, 33, '#707070', 13), (425, 33, '#707070', 13)]
    for i, (x, y, fill, size) in enumerate(header):
        out.append('<text id="%s_text_%d" transform="matrix(1 0 0 1 %s %s)" fill="%s" font-family="UniversNextPro-Light" font-size="%dpx">-</text>'
                   % (PREFIX, i, num(x), num(y), fill, size))
    for x, y, g, r in stars():
        out.append('<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="%s" stroke-width="0.1"/>' % (num(x), num(y), num(r), g, g))

    place = 'transform="matrix(1 0 0 1 %d %d)"' % (CX, CY)
    out.append('<circle id="%s_node_%d" %s cx="0" cy="0" r="32" fill="#000000" stroke="#323232" stroke-width="0.5"/>'
               % (PREFIX, NODE_DISC, place))
    ring = lambda r: [(r * math.cos(2 * math.pi * k / POINTS), r * math.sin(2 * math.pi * k / POINTS)) for k in range(POINTS)]
    out.append(path(ring(32), id='%s_node_%d' % (PREFIX, NODE_DAY), transform='matrix(1 0 0 1 %d %d)' % (CX, CY),
                    fill='#646464', stroke='#646464', stroke_width=0.5))
    for m in range(MERIDIANS):
        out.append(path(ring(30), id='%s_node_%d' % (PREFIX, NODE_MERIDIANS + m), transform='matrix(1 0 0 1 %d %d)' % (CX, CY),
                        fill='#3C3C3C', stroke='#3C3C3C', stroke_width=0.25))
    out.append(path([(-0.4, -AXIS_HALF), (0.4, -AXIS_HALF), (0.4, AXIS_HALF), (-0.4, AXIS_HALF)],
                    id='%s_node_%d' % (PREFIX, NODE_AXIS), transform='matrix(1 0 0 1 %d %d)' % (CX, CY),
                    fill='#969696', stroke='#969696', stroke_width=0.25))
    # The dashes: 3 px long, trailing back from the origin; display.cpp moves, turns and
    # scales each one (to 6 px) into a stretch of line.
    for i in range(specks):
        out.append(path([(0, -0.3), (-3, -0.3), (-3, 0.3), (0, 0.3)], id='%s_node_%d' % (PREFIX, NODE_SPECKS + i),
                        transform='matrix(1 0 0 1 640 320)', fill='#4B4B4B', stroke='#4B4B4B', stroke_width=0.1))
    # The dots: discs of radius 2 px, scaled to size (a node's scale is at most 2).
    for i in range(DOTS):
        out.append('<circle id="%s_node_%d" transform="matrix(1 0 0 1 640 320)" cx="0" cy="0" r="2" fill="#4B4B4B" stroke="#4B4B4B" stroke-width="0.1"/>'
                   % (PREFIX, NODE_DOTS + i))
    for i, g in enumerate(GREYS):
        c = '#%02X%02X%02X' % (g, g, g)
        out.append('<circle id="%s_palette_%d" cx="640" cy="0" r="1" fill="%s" stroke="%s" stroke-width="0.25"/>' % (PREFIX, i, c, c))
    out.append('<circle id="%s_palette_%d" cx="640" cy="0" r="1" fill="#000000" stroke="#323232" stroke-width="0.5"/>' % (PREFIX, NIGHT))
    for entry, edge in ((EDGE, '#FFFFFF'), (EDGE_MID, '#AFAFAF'), (EDGE_DIM, '#646464')):
        out.append('<circle id="%s_palette_%d" cx="640" cy="0" r="1" fill="#000000" stroke="%s" stroke-width="0.8"/>' % (PREFIX, entry, edge))
    out.append('</g>')
    open('panel.svg.fragment', 'w').write('\n'.join(out) + '\n')

    rows = lambda table: ',\n    '.join('{%s}' % ', '.join(str(v) for v in s) for s in table)
    h = []
    w = h.append
    w('// Generated by tools/make_panel.py: where display.cpp places the artwork, in')
    w('// pixels unless named Q4, Q10 or tenths (of a degree).')
    w('#pragma once')
    w('#include <opxy/types.hpp>')
    w('')
    w('namespace earthgrain { namespace panel {')
    w('const opxy::u32 kNodes = %d, kTexts = %d, kPalette = %d, kPoints = %d;' % (nodes, texts, len(GREYS) + 4, POINTS))
    w('enum Node { kNodeDay = %d, kNodeMeridians = %d, kNodeBars = %d, kNodeDisc = %d, kNodeAxis = %d, kNodeSpecks = %d, kNodeDots = %d };'
      % (NODE_DAY, NODE_MERIDIANS, NODE_BARS, NODE_DISC, NODE_AXIS, NODE_SPECKS, NODE_DOTS))
    w('const opxy::u32 kMeridians = %d, kSpecks = %d, kDots = %d;' % (MERIDIANS, specks, DOTS))
    w('const opxy::i32 kAxisHalf = %d;' % AXIS_HALF)
    w('const opxy::i32 kCx = %d, kCy = %d, kRadius = %d;' % (CX, CY, R))
    w('const opxy::i32 kLeanTenths = %d, kTipTenths = %d;' % (round(LEAN * 10), round(TIP * 10)))
    w('// The day side: the sun\'s nearness to us (Q10) and its direction on the screen, y up, Q10.')
    w('const opxy::i32 kSunNear = %d, kSunX = %d, kSunY = %d;' % (round(sz * 1024), round(sun[0] * 1024), round(sun[1] * 1024)))
    w('// Palette: greys 0..%d (their levels), then the night disc, then night with a bright, middling and dim edge.' % (len(GREYS) - 1))
    w('const opxy::u32 kGreys = %d, kShadeNight = %d, kShadeEdge = %d, kShadeEdgeMid = %d, kShadeEdgeDim = %d;' % (len(GREYS), NIGHT, EDGE, EDGE_MID, EDGE_DIM))
    w('const opxy::u8 kGreyLevel[%d] = {%s};' % (len(GREYS), ', '.join(str(g) for g in GREYS)))
    w('// Base greys: the day side, the limb, the meridians and the axis.')
    w('const opxy::i32 kDayGrey = 0x64, kLimbGrey = 0x32, kMeridianGrey = 0x3C, kAxisGrey = 0x96;')
    w('// Size: grains 20 ms x 60^size, in milliseconds across the control.')
    w('const opxy::u16 kSizeMs[33] = {%s};' % ', '.join(str(round(20 * 60 ** (i / 32))) for i in range(33)))
    w('// e^-x for x = 0..8 in steps of 1/4, Q15.')
    w('const opxy::u16 kDecay[33] = {%s};' % ', '.join(str(round(32767 * math.exp(-i / 4))) for i in range(33)))
    w('// sin over a whole turn in 1024 steps, Q10 (display.cpp\'s fast sine).')
    w('const opxy::i16 kSine[1024] = {%s};' % ', '.join(str(round(1024 * math.sin(2 * math.pi * i / 1024))) for i in range(1024)))
    w('// The palette grey nearest each level 0..255.')
    w('const opxy::u8 kGreyOf[256] = {%s};' % ', '.join(str(min(range(len(GREYS)), key=lambda k: abs(GREYS[k] - v))) for v in range(256)))
    w('}}')
    open('panel.h', 'w').write('\n'.join(h) + '\n')

    definition = json.load(open('definition.json'))
    definition['resources']['display'] = {'stateBytes': STATE_BYTES, 'stateAlignment': 4, 'paths': 1 + MERIDIANS, 'points': POINTS,
                                          'nodes': nodes, 'texts': texts, 'palette': len(GREYS) + 4}
    open('definition.json', 'w').write(json.dumps(definition, indent=2) + '\n')
    print('nodes %d (dashes %d, dots %d), texts %d, palette %d' % (nodes, specks, DOTS, texts, len(GREYS) + 4))


if __name__ == '__main__':
    main()
