"""
rsc_normalmap.py  —  Extract terrain textures and static-object textures from a
Carnivores 2 .RSC file and generate tangent-space normal map PNG siblings for
the PBR override system.

Usage:
    python rsc_normalmap.py <AREA1.RSC> [--strength N]

Output files (placed alongside the .RSC so the engine finds them via ProjectName):
    <stem>_tex_00_normal.png  ...  <stem>_tex_NN_normal.png   (terrain tiles)
    <stem>_obj_00_normal.png  ...  <stem>_obj_MM_normal.png   (static objects)

.RSC file layout (from Resources.cpp / Hunt.h):
  offset 0  : int32 tc   — terrain texture count
  offset 4  : int32 mc   — static model count
  offset 8  : int32[3][3] FadeRGB    (36 bytes)
  offset 44 : int32[3][3] TransRGB   (36 bytes)
  offset 80 : tc * (128*128*2) bytes — terrain textures (128x128 RGB555)
  then      : mc * object records
              each record:
                64 bytes   ObjInfo (flags at byte 28, ofANIMATED=0x80000000)
                LoadModel  (VCount:4, FCount:4, OCount:4, TextureSize:4,
                            FCount*64 faces, VCount*16 verts, OCount*48 objs,
                            TextureSize bytes texture [256px wide, RGB555])
                128*128*2 bytes   BMP-model billboard texture (skipped)
                if ofANIMATED:
                    4+4+4+4 bytes header, then vc*(FramesCount+1)*6 bytes data
  then      : 3 * 256*256*2 bytes  sky pictures (all 3 day variants)
  then      : 128*128 bytes        sky height map
  then      : int32 FgCount + FgCount*sizeof(TFogEntity) bytes  fog list
"""

import struct
import sys
import os
import argparse
import numpy as np
from PIL import Image


# ---------------------------------------------------------------------------
# RSC binary helpers
# ---------------------------------------------------------------------------

def ru32(f):
    return struct.unpack('<I', f.read(4))[0]

def ri32(f):
    return struct.unpack('<i', f.read(4))[0]

def skip(f, n):
    f.seek(n, 1)  # relative seek


# ---------------------------------------------------------------------------
# RGB555 → numpy uint8 RGB
# ---------------------------------------------------------------------------

def decode_rgb555(raw: bytes, width: int) -> np.ndarray:
    n_pixels = len(raw) // 2
    height = n_pixels // width
    if height == 0:
        return None
    words = np.frombuffer(raw[:width * height * 2], dtype='<u2')
    r = ((words >> 10) & 0x1F) * 8
    g = ((words >>  5) & 0x1F) * 8
    b = ((words >>  0) & 0x1F) * 8
    return np.stack([r, g, b], axis=-1).astype(np.uint8).reshape(height, width, 3)


# ---------------------------------------------------------------------------
# Normal map generation (reused from car_normalmap.py)
# ---------------------------------------------------------------------------

def luminance(rgb: np.ndarray) -> np.ndarray:
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0


def generate_normal_map(rgb: np.ndarray, strength: float = 6.0) -> Image.Image:
    height = luminance(rgb).astype(np.float32)
    h_pad  = np.pad(height, 1, mode='wrap')
    dzdx   = (h_pad[1:-1, 2:] - h_pad[1:-1, :-2]) * 0.5 * strength
    dzdy   = (h_pad[2:, 1:-1] - h_pad[:-2, 1:-1]) * 0.5 * strength
    nx = -dzdx; ny = -dzdy; nz = np.ones_like(nx)
    length = np.sqrt(nx*nx + ny*ny + nz*nz)
    nx /= length; ny /= length; nz /= length

    r  = np.clip((nx * 0.5 + 0.5) * 255, 0, 255).astype(np.uint8)
    g  = np.clip((ny * 0.5 + 0.5) * 255, 0, 255).astype(np.uint8)
    b  = np.clip((nz * 0.5 + 0.5) * 255, 0, 255).astype(np.uint8)

    lo, hi = height.min(), height.max()
    a = ((height - lo) / max(hi - lo, 1e-6) * 255).astype(np.uint8) if hi > lo else \
        np.full(height.shape, 128, dtype=np.uint8)

    return Image.fromarray(np.dstack([r, g, b, a]), 'RGBA')


def save_normal_map(rgb: np.ndarray, path: str, strength: float):
    img = generate_normal_map(rgb, strength)
    img.save(path)
    print(f"  Saved: {path}  ({img.size[0]}x{img.size[1]})")


# ---------------------------------------------------------------------------
# .RSC parser
# ---------------------------------------------------------------------------

OF_ANIMATED = 0x80000000


def skip_load_model(f):
    """Skip a LoadModel block and return the raw texture bytes."""
    vcount      = ru32(f)
    fcount      = ru32(f)
    ocount      = ru32(f)
    texture_size = ru32(f)            # raw bytes stored in file
    skip(f, fcount * 64)             # face data
    skip(f, vcount * 16)             # vertex data
    skip(f, ocount * 48)             # object/bone data
    raw = f.read(texture_size)
    return raw


def skip_animation(f):
    """Skip a LoadAnimation block."""
    vc          = ru32(f)
    _vc2        = ru32(f)            # read twice in original code
    _kps        = ru32(f)
    frames      = ru32(f)            # stored as N, actual count = N+1
    skip(f, vc * (frames + 1) * 6)  # int16[vc * (frames+1) * 3] animation frames


def process_rsc(rsc_path: str, strength: float):
    stem = os.path.splitext(rsc_path)[0]   # e.g. "HUNTDAT/AREAS/AREA1"

    with open(rsc_path, 'rb') as f:
        # --- header ---
        tc = ri32(f)
        mc = ri32(f)
        skip(f, 36 + 36)   # FadeRGB + TransRGB

        print(f"\n{os.path.basename(rsc_path)}: {tc} terrain textures, {mc} objects")

        # --- terrain textures ---
        print(f"Terrain textures:")
        for tt in range(tc):
            raw = f.read(128 * 128 * 2)
            rgb = decode_rgb555(raw, 128)
            if rgb is None:
                print(f"  tex_{tt:02d}: empty, skipped")
                continue
            out = f"{stem}_tex_{tt:02d}_normal.png"
            save_normal_map(rgb, out, strength)

        # --- static objects ---
        print(f"Objects:")
        for mm in range(mc):
            # ObjInfo struct (64 bytes); flags is at byte offset 28
            info_bytes = f.read(64)
            flags = struct.unpack_from('<I', info_bytes, 28)[0]

            raw = skip_load_model(f)
            rgb = decode_rgb555(raw, 256)

            # skip BMP billboard texture
            skip(f, 128 * 128 * 2)

            # skip animation data if present
            if flags & OF_ANIMATED:
                skip_animation(f)

            if rgb is None:
                print(f"  obj_{mm:02d}: empty texture, skipped")
                continue
            out = f"{stem}_obj_{mm:02d}_normal.png"
            save_normal_map(rgb, out, strength)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate PBR normal maps from terrain and object textures in a .RSC file")
    parser.add_argument('rsc', nargs='+', help='.RSC file(s) to process')
    parser.add_argument('--strength', type=float, default=6.0,
                        help='Normal map strength (default 6.0)')
    args = parser.parse_args()

    for rsc_path in args.rsc:
        if not os.path.exists(rsc_path):
            print(f"Not found: {rsc_path}", file=sys.stderr)
            continue
        try:
            process_rsc(rsc_path, args.strength)
        except Exception as e:
            print(f"Error processing {rsc_path}: {e}", file=sys.stderr)
            import traceback; traceback.print_exc()

    print("\nDone.")


if __name__ == '__main__':
    main()
