"""Pack authored texture atlases; models, UVs and native headers stay in the ROM.

Requires Pillow for the offline authoring step. The game reads only the small
CI8 payloads. No ROM, extracted texture, screenshot or model is an input here.
"""
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
import zipfile

from PIL import Image

SOURCE = Path(__file__).resolve().parents[1] / 'mods/more-characters-skins'
SLOTS = (('head', 64, 32), ('torso', 64, 32), ('body', 64, 32), ('far', 32, 16))

# These two helmets retain their existing atlas artwork. The other riders
# require dedicated mirrored-half-head artwork declared by head-layout.json.
ATLAS_HEADS = frozenset(('ghost-rider', 'master-chief'))
# Revised faces are packed last to preserve calibrated body colors. Marcus's
# original head remains an offline palette reference, never a separate runtime asset.
PACKED_HEADS = frozenset(('doom-guy', 'marcus-fenix'))
HEAD_SIZE = (38, 32)
BODY_RIDERS = frozenset(('doom-guy', 'vin-diesel', 'link', 'marcus-fenix',
                         'riddick', 'dominic-santiago', 'green-pants-rider'))
# The lower tiles share space with faces. Include their filtering borders.
PROTECTED_HEADS = {'head': (26, 0, 64, 32), 'body': (35, 10, 64, 32),
                   'far': (17, 5, 28, 16)}


def load_body_layout():
    """Read reviewed texel copies; no model data or ROM is needed to pack them."""
    path = SOURCE / 'body-layout.json'
    document = json.loads(path.read_text())
    if (set(document) != {'version', 'description', 'characters'} or
            type(document['version']) is not int or document['version'] != 1 or
            not isinstance(document['characters'], dict) or
            set(document['characters']) != BODY_RIDERS):
        raise ValueError('body-layout.json must declare the seven calibrated riders')
    sizes = {name: (w, h) for name, w, h in SLOTS}
    for identity, character in document['characters'].items():
        if set(character) != {'authoredAtlasSHA256', 'slots'}:
            raise ValueError(f'{identity}: invalid body-layout fields')
        digest = hashlib.sha256((SOURCE / 'art' / (identity + '.png')).read_bytes()).hexdigest()
        if character['authoredAtlasSHA256'] != digest:
            raise ValueError(f'{identity}: body artwork changed; review its calibration')
        if not character['slots'] or set(character['slots']) - set(sizes):
            raise ValueError(f'{identity}: invalid body-layout slots')
        for name, entry in character['slots'].items():
            if set(entry) != {'inputSHA256', 'outputSHA256', 'copies', 'paletteIndices'}:
                raise ValueError(f'{identity}/{name}: invalid texel-copy fields')
            width, height = sizes[name]
            if (not isinstance(entry['copies'], list) or not isinstance(entry['paletteIndices'], list) or
                    len(entry['copies']) + len(entry['paletteIndices']) > width * height):
                raise ValueError(f'{identity}/{name}: invalid texel-copy count')
            seen = set()
            operations = [(pair, width * height) for pair in entry['copies']]
            operations += [(pair, 256) for pair in entry['paletteIndices']]
            for pair, source_limit in operations:
                if (not isinstance(pair, list) or len(pair) != 2 or
                        type(pair[0]) is not int or not 0 <= pair[0] < width * height or
                        type(pair[1]) is not int or not 0 <= pair[1] < source_limit):
                    raise ValueError(f'{identity}/{name}: texel copy outside texture')
                destination, _ = pair
                if destination in seen:
                    raise ValueError(f'{identity}/{name}: repeated destination texel')
                seen.add(destination)
                if name in PROTECTED_HEADS:
                    x0, y0, x1, y1 = PROTECTED_HEADS[name]
                    x, y = destination % width, destination // width
                    if x0 <= x < x1 and y0 <= y < y1:
                        raise ValueError(f'{identity}/{name}: copy would alter a protected head')
    return document['characters'], hashlib.sha256(path.read_bytes()).hexdigest()


def apply_body_layout(payload, entry):
    """Copy immutable CI8 indices after quantization so face colors cannot drift."""
    if entry is None:
        return payload
    if hashlib.sha256(payload).hexdigest() != entry['inputSHA256']:
        raise ValueError('Body palette or packing changed; recalibrate before packaging')
    result = bytearray(payload)
    for destination, source in entry['copies']:
        result[destination] = payload[source]
    # Palette reduction may leave a reviewed color without a source texel.
    # Select that existing color by index; never modify the palette itself.
    for destination, index in entry['paletteIndices']:
        result[destination] = index
    result = bytes(result)
    if hashlib.sha256(result).hexdigest() != entry['outputSHA256']:
        raise ValueError('Body correction does not match the reviewed texture')
    return result


def replace_packed_head(payload, slot, head):
    """Allocate face colors without changing any decoded body texel."""
    regions = {0: (26, 0, 64, 32), 2: (36, 11, 63, 30), 3: (18, 6, 27, 16)}
    if head is None or slot not in regions:
        return payload
    _, width, height = SLOTS[slot]
    pixels = width * height
    x0, y0, x1, y1 = regions[slot]
    region = [y * width + x for y in range(y0, y1) for x in range(x0, x1)]
    region_set = set(region)
    result = bytearray(payload)
    palette = struct.unpack('>256H', payload[pixels:])
    # Quantization can assign several indices to the same final 5-bit color.
    # Merge those aliases outside the face to free slots without recoloring it.
    canonical = {}
    for offset, index in enumerate(payload[:pixels]):
        if offset not in region_set:
            result[offset] = canonical.setdefault(palette[index], index)
    used_by_body = set(canonical.values())
    available = [index for index in range(256) if index not in used_by_body]
    if len(available) < 16:
        raise ValueError('Not enough unused palette entries for a separate head')
    image = head.resize((x1 - x0, y1 - y0), Image.Resampling.BOX)
    indexed = image.quantize(colors=len(available), method=Image.Quantize.MEDIANCUT,
                             dither=Image.Dither.NONE)
    colors = indexed.getpalette()
    for index in set(indexed.tobytes()):
        r, g, b = colors[index * 3:index * 3 + 3]
        word = (((r * 31 + 127) // 255 << 11) | ((g * 31 + 127) // 255 << 6) |
                ((b * 31 + 127) // 255 << 1) | 1)
        offset = pixels + available[index] * 2
        result[offset:offset + 2] = word.to_bytes(2, 'big')
    for offset, index in zip(region, indexed.tobytes()):
        result[offset] = available[index]
    return bytes(result)


def validate_head_layout(layout, label='head'):
    """Validate optional horizontal landmarks in the logical 38x32 strip."""
    if layout is None:
        return {}
    if not isinstance(layout, dict) or set(layout) - {'file', 'source_x', 'target_x'}:
        raise ValueError(f'{label}: invalid head-layout fields')
    if ('source_x' in layout) != ('target_x' in layout):
        raise ValueError(f'{label}: source_x and target_x must be provided together')
    if 'source_x' not in layout:
        return {}
    source, target = layout['source_x'], layout['target_x']
    if (not isinstance(source, list) or not isinstance(target, list) or
            not 2 <= len(source) <= 32 or len(source) != len(target)):
        raise ValueError(f'{label}: matching landmark arrays need 2 to 32 entries')
    for points in (source, target):
        if any(isinstance(v, bool) or not isinstance(v, (int, float)) or
               not math.isfinite(v) for v in points):
            raise ValueError(f'{label}: landmarks must be finite numbers')
        if (points[0] != 0 or points[-1] != HEAD_SIZE[0] or
                any(a >= b for a, b in zip(points, points[1:]))):
            raise ValueError(f'{label}: landmarks must increase strictly from 0 to 38')
    return {'source_x': list(source), 'target_x': list(target)}


def fit_head_strip(head_strip, head_layout=None):
    """Resample authored coordinates only; never paint or change the native UVs."""
    layout = validate_head_layout(head_layout)
    image = head_strip.convert('RGB')
    if image.width < HEAD_SIZE[0] or image.height < HEAD_SIZE[1]:
        raise ValueError('Head artwork must be at least 38x32 pixels')
    if layout and layout['source_x'] != layout['target_x']:
        source, target = layout['source_x'], layout['target_x']

        def source_x(x):
            logical = x * HEAD_SIZE[0] / image.width
            for i in range(1, len(target)):
                if logical <= target[i]:
                    fraction = (logical - target[i - 1]) / (target[i] - target[i - 1])
                    logical_source = source[i - 1] + fraction * (source[i] - source[i - 1])
                    return logical_source * image.width / HEAD_SIZE[0]
            return image.width

        # One-pixel-wide mesh strips keep fractional landmarks continuous.
        # Fit at authored resolution, then reduce once to the native size.
        mesh = []
        for x in range(image.width):
            left, right = source_x(x), source_x(x + 1)
            mesh.append(((x, 0, x + 1, image.height),
                         (left, 0, left, image.height, right, image.height, right, 0)))
        image = image.transform(image.size, Image.Transform.MESH, mesh, Image.Resampling.BILINEAR)
    return image.resize(HEAD_SIZE, Image.Resampling.BOX)


def load_head_assets(roster):
    """Fail closed for missing declared art; a partial roster is not a release."""
    path = SOURCE / 'head-layout.json'
    document = json.loads(path.read_text())
    if (not isinstance(document, dict) or set(document) != {'version', 'heads'} or
            type(document['version']) is not int or document['version'] != 1 or
            not isinstance(document['heads'], dict)):
        raise ValueError('head-layout.json must contain version 1 and a heads object')
    # New skins can author the mirrored head directly in their atlas. Existing
    # calibrated head strips stay mandatory unless explicitly declared here.
    expected = {c['id'] for c in roster if not c.get('atlas_head', False)} - ATLAS_HEADS
    if set(document['heads']) != expected:
        raise ValueError('head-layout.json must declare every non-helmet rider exactly once')
    heads, layouts, hashes = {}, {}, {}
    for identity, entry in document['heads'].items():
        layout = validate_head_layout(entry, identity)
        relative = f'art/heads/{identity}.png'
        if entry.get('file') != relative:
            raise ValueError(f'{identity}: head file must be {relative}')
        file = SOURCE / relative
        with Image.open(file) as original:
            if (original.format != 'PNG' or original.width < HEAD_SIZE[0] or
                    original.height < HEAD_SIZE[1]):
                raise ValueError(f'{identity}: head must be a PNG of at least 38x32 pixels')
            if original.convert('RGBA').getchannel('A').getextrema() != (255, 255):
                raise ValueError(f'{identity}: head artwork must be opaque')
            heads[identity] = original.convert('RGB')
        layouts[identity] = layout
        hashes[identity] = hashlib.sha256(file.read_bytes()).hexdigest()
    return heads, layouts, hashes, hashlib.sha256(path.read_bytes()).hexdigest()


def copy_region(source, box, destination, target):
    """Resample an authored island into the matching native texture island."""
    width, height = target[2] - target[0], target[3] - target[1]
    destination.paste(source.crop(box).resize((width, height), Image.Resampling.BOX), target[:2])


def native_tiles(atlas, identity='', *, head_strip=None, head_layout=None,
                 head_back_first=False, torso_back_first=False):
    tiles = []
    for slot, (_, width, height) in enumerate(SLOTS):
        x, y = slot % 2, slot // 2
        box = (round(x * atlas.width / 2), round(y * atlas.height / 2),
               round((x + 1) * atlas.width / 2), round((y + 1) * atlas.height / 2))
        tiles.append(atlas.crop(box).convert('RGB').resize((width, height), Image.Resampling.BOX))

    # The native head mirrors its front UV strip. Dedicated authored strips fit
    # that layout; the three unchanged helmets keep their original atlas area.
    if head_strip is None:
        if head_layout:
            raise ValueError('Head landmarks require a dedicated head image')
        head = tiles[0].crop((26, 0, 64, 32))
        if head_back_first:
            head = head.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    else:
        if head_back_first:
            head_strip = head_strip.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
        head = fit_head_strip(head_strip, head_layout)
    head = head.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    tiles[0].paste(head, (26, 0))
    tiles[1] = tiles[1].transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    if torso_back_first:
        # Native torso U=0..31 is the front. Reorder authored islands, rather
        # than changing shared mesh UVs or placing faces on the back of heads.
        torso = tiles[1].copy()
        tiles[1].paste(torso.crop((32, 0, 64, 32)), (0, 0))
        tiles[1].paste(torso.crop((0, 0, 32, 32)), (32, 0))

    if identity in ('lara-croft', 'duke-nukem', 'serious-sam'):
        # These atlases painted limbs shoulder-to-hand / hip-to-boot, whereas
        # native limb T runs from the extremity upward. Keep head UVs separate.
        arms = tiles[0].crop((0, 0, 13, 32)).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        leg_box = (14, 0, 19, 32) if identity == 'lara-croft' else (14, 0, 25, 32)
        legs = tiles[0].crop(leg_box).resize((13, 32), Image.Resampling.BOX).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        if identity == 'serious-sam':
            # His sneakers occupy the native foot band, rather than boot-height calves.
            original = legs.copy()
            copy_region(original, (0, 2, 13, 6), legs, (0, 0, 13, 2))
            copy_region(original, (0, 8, 13, 32), legs, (0, 2, 13, 32))
        tiles[0].paste(arms, (0, 0))
        tiles[0].paste(legs, (13, 0))
        # Native chest UVs stretch the top edge into the neck. Fit the garment
        # below the authored collar rather than stretching a large skin patch.
        top = 28 if identity == 'serious-sam' else 24
        copy_region(tiles[1], (0, 0, 32, top), tiles[1], (0, 0, 32, 32))

    # Share the same face and chest at every distance. Keep each lower tile's
    # authored limb regions; their native layouts differ from the near model.
    copy_region(tiles[1], (0, 0, 32, 32), tiles[2], (0, 0, 13, 32))
    copy_region(tiles[1], (32, 0, 64, 32), tiles[2], (13, 0, 26, 32))
    copy_region(tiles[0], (26, 0, 64, 32), tiles[2], (36, 11, 63, 30))

    if identity in ('lara-croft', 'duke-nukem', 'serious-sam'):
        copy_region(tiles[0], (0, 0, 13, 32), tiles[2], (26, 0, 36, 32))
        # The lower model puts shins and thighs below its face, not in the
        # near-model leg column. Keep these disjoint from its head rectangle.
        # At lower detail limb length runs along U (near detail uses T).
        shin = tiles[0].crop((13, 0, 26, 11)).transpose(Image.Transpose.TRANSPOSE)
        thigh = tiles[0].crop((13, 13, 26, 32)).transpose(Image.Transpose.TRANSPOSE)
        tiles[2].paste(shin.resize((16, 11), Image.Resampling.BOX), (36, 0))
        tiles[2].paste(thigh.resize((12, 11), Image.Resampling.BOX), (52, 0))

    # Native limb end caps use the otherwise spare texels beside the head.
    # Give those caps the authored trouser color, rather than face/neck pixels.
    copy_region(tiles[2], (52, 7, 56, 8), tiles[2], (37, 30, 41, 31))
    copy_region(tiles[2], (0, 0, 26, 32), tiles[3], (0, 0, 13, 16))
    copy_region(tiles[0], (26, 0, 64, 32), tiles[3], (18, 6, 27, 16))
    return tiles


def convert(atlas, slot, identity='', *, prepared=None, body_layout=None, packed_head=None):
    """Pack one native-size CI8 tile; optional prepared tiles avoid repeat work."""
    name, width, height = SLOTS[slot]
    image = (prepared if prepared is not None else native_tiles(atlas, identity))[slot]
    indexed = image.quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                             dither=Image.Dither.NONE)
    palette = indexed.getpalette()
    words = []
    for i in range(256):
        r, g, b = palette[i * 3:i * 3 + 3] if i * 3 + 3 <= len(palette) else (0, 0, 0)
        words.append(((r * 31 + 127) // 255 << 11) |
                     ((g * 31 + 127) // 255 << 6) |
                     ((b * 31 + 127) // 255 << 1) | 1)
    payload = indexed.tobytes() + struct.pack('>256H', *words)
    payload = apply_body_layout(payload, body_layout)
    payload = replace_packed_head(payload, slot, packed_head)
    words = struct.unpack('>256H', payload[width * height:])
    # Preview the actual 5-bit console palette, not the higher-resolution art.
    preview = Image.new('RGB', (width, height))
    colors = [(((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31,
               ((v >> 1) & 31) * 255 // 31) for v in words]
    preview.putdata([colors[i] for i in payload[:width * height]])
    assert len(payload) == width * height + 512
    return name, payload, preview


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--character', help='Preview one authored character without packaging a partial mod')
    parser.add_argument('--package', type=Path)
    args = parser.parse_args()
    assert not (args.character and args.package), 'Do not distribute a partial roster.'
    roster = json.loads((SOURCE / 'roster.json').read_text())['characters']
    heads, head_layouts, head_hashes, layout_hash = load_head_assets(roster)
    bodies, body_layout_hash = load_body_layout()
    selected = [c for c in roster if not args.character or c['id'] == args.character]
    assert selected, 'Unknown character.'
    args.out.mkdir(parents=True, exist_ok=True)
    contents, records, art_hashes = {}, [], {}
    for character in selected:
        identity = character['id']
        art = SOURCE / 'art' / (identity + '.png')
        atlas = Image.open(art)
        assert atlas.width >= 128 and atlas.height >= 64 and abs(atlas.width / atlas.height - 2) < .02, art
        art_hashes[identity] = hashlib.sha256(art.read_bytes()).hexdigest()
        files = []
        late_head = identity in PACKED_HEADS
        prepared = native_tiles(atlas, identity, head_strip=(Image.open(SOURCE / 'art/heads/marcus-fenix-body-palette.png')
                                              if identity == 'marcus-fenix' else None if late_head else heads.get(identity)),
                                head_layout=None if late_head and identity != 'marcus-fenix' else head_layouts.get(identity),
                                head_back_first=character.get('head_back_first', False),
                                torso_back_first=character.get('torso_back_first', False))
        packed_head = (fit_head_strip(heads[identity], head_layouts.get(identity))
                       .transpose(Image.Transpose.FLIP_TOP_BOTTOM)) if late_head else None
        for slot in range(4):
            body_layout = bodies.get(identity, {}).get('slots', {}).get(SLOTS[slot][0])
            name, payload, preview = convert(atlas, slot, identity, prepared=prepared,
                                             body_layout=body_layout, packed_head=packed_head)
            filename = identity + '-' + name + '.ci8'
            contents[filename] = payload
            files.append(filename)
            preview.save(args.out / (identity + '-' + name + '.png'))
        record = dict(id=identity, name=character['name'], donor=character['donor'], textures=files)
        if character.get('turtle_shell', False):
            record['turtle_shell'] = True
        if character.get('dual_head', False):
            record['dual_head'] = True
        records.append(record)
    descriptor = dict(format='rr64-rider-skins', version=1, characters=records)
    contents['rr64-rider-skins.json'] = (json.dumps(descriptor, indent=2) + '\n').encode()
    for name in ('mod.json', 'README.txt'):
        contents[name] = (SOURCE / name).read_bytes()
    for name, payload in contents.items():
        (args.out / name).write_bytes(payload)
    if args.package:
        args.package.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.package, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, payload in sorted(contents.items()):
                info = zipfile.ZipInfo(name, (2026, 10, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                archive.writestr(info, payload)
        with zipfile.ZipFile(args.package) as archive:
            assert archive.testzip() is None and set(archive.namelist()) == set(contents)
            assert all(archive.read(n) == value for n, value in contents.items())
    report = dict(characters=len(records), authoredAtlases=art_hashes,
                  files={n: hashlib.sha256(v).hexdigest() for n, v in contents.items()},
                  texturePayloadBytes=sum(len(v) for n, v in contents.items() if n.endswith('.ci8')),
                  sourceNativeUVsUnchanged=True,
                  separateCheekUVs=[c['id'] for c in selected if c.get('dual_head')],
                  authoredToNativeVOrigin=True,
                  atlasPackingVersion=5, sourceArtUnchanged=True,
                  bodyLayoutSHA256=body_layout_hash,
                  bodyCorrections=[c['id'] for c in selected if c['id'] in bodies],
                  originalPalettesAndHeadRegionsPreserved=not any(c['id'] in PACKED_HEADS for c in selected),
                  bodyColorsPreserved=True,
                  packedHeadReplacements=[c['id'] for c in selected if c['id'] in PACKED_HEADS],
                  authoredHeadStrips={c['id']: head_hashes[c['id']] for c in selected if c['id'] in head_hashes},
                  headLayoutSHA256=layout_hash,
                  headCalibration={c['id']: head_layouts[c['id']] for c in selected if c['id'] in head_layouts},
                  lowerFaceAndChestFromNear=True, lowerLimbEndCapsPreserved=True,
                  romRead=False, modelOrNativeHeaderIncluded=False,
                  package=str(args.package) if args.package else None)
    (args.out / 'authoring-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"Converted {len(records)} authored skins; {report['texturePayloadBytes']} texture bytes.")


if __name__ == '__main__':
    main()
