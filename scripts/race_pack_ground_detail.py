"""Resolve imported terrain's incompatible near/far camera representations.

This is an offline importer pass, never a runtime geometry or collision patch.
The caller must prove eligible coarse/detail leaf pairs from donor camera roots
and provide the collision triangles accepted by that donor's initialization.
By default only opaque upward ground is changed. An explicit, donor-verified
extension admits sloped distant sides and neighboring close road surfaces.
Accepted physical geometry, vertical faces and uncovered regions are retained.
Rational polygon subtraction preserves uncovered regions and vertex attributes.

No ROM or texture data is included in this module. The private Frappe integration
and provenance are documented in docs/frappe-ground-detail.md.
"""
from collections import Counter
from fractions import Fraction as F
import copy

def side(a, b, p):
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])

def area(p):
    return abs(sum((p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p)))) / 2) if len(p) >= 3 else F(0)

def signed(p):
    return sum((p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p))))

def clean(p):
    out = []
    for v in p:
        if not out or v != out[-1]:
            out.append(v)
    if len(out) > 1 and out[0] == out[-1]:
        out.pop()
    return out

def split(poly, a, b):
    inside = []
    outside = []
    prev = poly[-1]
    dp = side(a, b, prev)
    for cur in poly:
        dc = side(a, b, cur)
        if dp > 0 and dc < 0 or (dp < 0 and dc > 0):
            t = dp / (dp - dc)
            p = tuple((prev[k] + t * (cur[k] - prev[k]) for k in range(2)))
            inside.append(p)
            outside.append(p)
        if dc >= 0:
            inside.append(cur)
        if dc <= 0:
            outside.append(cur)
        prev, dp = (cur, dc)
    return (clean(inside), clean(outside))

def subtract(subject, clip):
    if not area(clip):
        return [subject]
    clip = list(clip)
    if signed(clip) < 0:
        clip.reverse()
    remaining = subject
    residual = []
    for i, a in enumerate(clip):
        if not area(remaining):
            break
        remaining, outside = split(remaining, a, clip[(i + 1) % len(clip)])
        if area(outside):
            residual.append(outside)
    # Disjoint outside halfplanes preserve every uncovered part of the source.
    assert sum(map(area, residual), F(0)) + area(remaining) == area(subject)
    return residual

def subtract_union(subject, clips):
    residual = [subject]
    for clip in clips:
        residual = [q for p in residual for q in subtract(p, clip)]
        if not residual:
            break
    return residual

def scalar(v):
    return int(v) if v.denominator == 1 else float(v)

def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])

def attributes(vertex):
    return vertex['position'] + vertex['texcoord_st'] + vertex['color_rgba']

def prefer_camera_ground_detail(before, paired, roots, accepted_collision_triangles,
                                *, include_sloping_sides=False,
                                allow_adjacent_close_ground=False,
                                nonphysical_replacements=None):
    """Return (corrected course, audit) without mutating inputs.

    ``paired`` maps proven coarse display-list leaves to their detailed leaves.
    ``roots`` maps each leaf to the set of donor section/direction render roots
    containing it. Adjacent physical road surfaces can cover coarse snow where
    their camera roots are likewise mutually exclusive; texture identity alone
    must not preserve a coarse snowbank that visually protrudes into the road.
    ``accepted_collision_triangles`` uses indices in ``before['vertices']``.
    The optional extension is for a proven terrain LOD boundary, not arbitrary
    overlapping objects: a simplified snowbank can span the adjacent close
    road, and can slope steeply or have a downward-facing side. The paired near
    leaf must still be mutually exclusive with its far leaf in donor cameras.
    ``nonphysical_replacements`` optionally maps a far leaf to specific authored
    river/void surface leaves. This must be justified separately: a far shoreline
    may extend into water beyond the real near shoreline. It changes only which
    surface is visible, never adds floor contact to water or a fall-through area.
    """

    def positions(t):
        return [before['vertices'][i]['position'] for i in t['vertices']]

    def key(t):
        return tuple(sorted((tuple(p) for p in positions(t))))

    def ground(t, *, coarse=False):
        a, b, c = positions(t)
        n = cross([b[k] - a[k] for k in range(3)], [c[k] - a[k] for k in range(3)])
        if include_sloping_sides:
            return n[1] != 0 if coarse else n[1] > 0
        return n[1] > 0 and n[1] >= max(abs(n[0]), abs(n[2]))

    def proven_near_pair(leaf):
        return any(roots.get(detail) and roots.get(leaf)
                   and not roots[detail] & roots[leaf]
                   for detail in paired.get(leaf, ()))

    def opaque(t):
        return not any(('XLU' in mode or 'TEX_EDGE' in mode for mode in before['materials'][t['material']]['render']))

    def poly(t):
        return [(F(p[0]), F(p[2])) for p in positions(t)]

    def bounds(p):
        return (min((v[0] for v in p)), max((v[0] for v in p)), min((v[1] for v in p)), max((v[1] for v in p)))

    def overlap(a, b):
        return a[0] < b[1] and b[0] < a[1] and (a[2] < b[3]) and (b[2] < a[3])
    accepted = {key(t) for t in accepted_collision_triangles}
    nonphysical_replacements = nonphysical_replacements or {}
    replacement_leaves = set().union(*nonphysical_replacements.values()) if nonphysical_replacements else set()
    details = []
    seen = set()
    for j, t in enumerate(before['triangles']):
        k = key(t)
        if (k not in accepted and t['display_list'] not in replacement_leaves
                or not ground(t) or not opaque(t)):
            continue
        unique = (k, t['display_list'])
        if unique in seen:
            continue
        seen.add(unique)
        p = poly(t)
        details.append((j, t, p, bounds(p)))
    after = copy.deepcopy(before)
    result, changes = ([], [])
    counts = Counter()
    cache = {}
    for i, t in enumerate(before['triangles']):
        leaf = t['display_list']
        if (leaf not in paired or not ground(t, coarse=True) or not opaque(t)
                or key(t) in accepted
                or (allow_adjacent_close_ground and not proven_near_pair(leaf))):
            result.append(t)
            continue
        subject = poly(t)
        box = bounds(subject)
        candidates = [
            (j, u, p) for j, u, p, b in details
            if overlap(box, b) and roots.get(leaf) and roots.get(u['display_list'])
            and (key(u) in accepted
                 or (proven_near_pair(leaf)
                     and u['display_list'] in nonphysical_replacements.get(leaf, ())))
            and (allow_adjacent_close_ground
                 or not roots[leaf] & roots[u['display_list']])
        ]
        candidates = [(j, u, p) for j, u, p in candidates if sum(map(area, subtract(subject, p)), F(0)) < area(subject)]
        if not candidates:
            result.append(t)
            continue
        residual = subtract_union(subject, [p for _, _, p in candidates])
        original_area = area(subject)
        residual_area = sum(map(area, residual), F(0))
        assert 0 <= residual_area < original_area
        row = {'triangle': i, 'leaf': leaf, 'detail_triangles': [j for j, _, _ in candidates], 'area': str(original_area), 'residual_area': str(residual_area), 'residual_triangles': 0}
        if residual_area:
            attrs = [list(map(F, attributes(before['vertices'][v]))) for v in t['vertices']]
            den = side(subject[0], subject[1], subject[2])
            for p in residual:
                for k in range(1, len(p) - 1):
                    points = [p[0], p[k], p[k + 1]]
                    if not area(points):
                        continue
                    indices = []
                    for point in points:
                        weights = [side(subject[1], subject[2], point) / den, side(subject[2], subject[0], point) / den, side(subject[0], subject[1], point) / den]
                        assert sum(weights) == 1 and all((v >= 0 for v in weights))
                        values = tuple((sum((weights[j] * attrs[j][c] for j in range(3))) for c in range(9)))
                        if values not in cache:
                            cache[values] = len(after['vertices'])
                            v = list(map(scalar, values))
                            after['vertices'].append({'position': v[:3], 'texcoord_st': v[3:5], 'color_rgba': v[5:]})
                        indices.append(cache[values])
                    result.append({**t, 'vertices': indices, 'ground_detail_source_triangle': i})
                    row['residual_triangles'] += 1
            counts['partially_clipped'] += 1
        else:
            counts['fully_covered'] += 1
        changes.append(row)
    after['triangles'] = result
    after['validation']['render_triangles'] = len(result)
    after['validation']['vertices'] = len(after['vertices'])
    for name in ('collision_triangles', 'collision_roots', 'path', 'paths', 'materials', 'textures'):
        assert after[name] == before[name]
    assert after['vertices'][:len(before['vertices'])] == before['vertices']
    return (after, {'counts': dict(counts), 'changes': changes, 'before_triangles': len(before['triangles']), 'after_triangles': len(result)})
