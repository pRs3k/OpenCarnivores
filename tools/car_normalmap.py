"""
car_normalmap.py  —  Extract texture from a Carnivores 2 .CAR file and
generate a tangent-space normal map PNG sibling for the PBR override system.

Usage:
    python car_normalmap.py <file.CAR> [--strength N] [--out path.png]

Output defaults to <stem>_normal.png next to the .CAR file, which is exactly
where Materials::TryRegisterSibling looks for it.

Normal map convention: RGB = (X+1)/2, (Y+1)/2, (Z+1)/2 in OpenGL tangent space.
Alpha channel encodes height (0-255) for parallax mapping support.

.CAR character texture layout (from LoadCharacterInfo in Resources.cpp):
  offset 0   : 32 bytes  ModelName
  offset 32  : 4  bytes  AniCount
  offset 36  : 4  bytes  SfxCount
  offset 40  : 4  bytes  VCount
  offset 44  : 4  bytes  FCount
  offset 48  : 4  bytes  TextureSize  (bytes of raw RGB555 data)
  offset 52  : FCount*64 bytes  face data
  offset ...  : VCount*16 bytes  vertex data
  offset ...  : TextureSize bytes  texture (256px wide, RGB555 little-endian words)
"""

import struct
import sys
import os
import argparse
import numpy as np
from PIL import Image


# ---------------------------------------------------------------------------
# .CAR parser
# ---------------------------------------------------------------------------

def read_u32(f):
    return struct.unpack('<I', f.read(4))[0]


def extract_car_texture(path: str) -> np.ndarray:
    """Return the texture as an (H, 256, 3) uint8 RGB array."""
    with open(path, 'rb') as f:
        f.read(32)           # ModelName
        f.read(4)            # AniCount
        f.read(4)            # SfxCount
        vcount = read_u32(f)
        fcount = read_u32(f)
        tex_size = read_u32(f)   # bytes of RGB555 data

        f.read(fcount * 64)  # face data
        f.read(vcount * 16)  # vertex data

        raw = f.read(tex_size)

    # Width is always 256 (512 bytes per row at 16 bpp)
    width = 256
    height = tex_size // (width * 2)
    if height == 0:
        raise ValueError(f"Unexpected texture size {tex_size}")

    # Decode RGB555: XRRRRRGGGGGBBBBB (little-endian)
    words = np.frombuffer(raw[:width * height * 2], dtype='<u2')
    r = ((words >> 10) & 0x1F) * 8   # 5-bit → 8-bit
    g = ((words >>  5) & 0x1F) * 8
    b = ((words >>  0) & 0x1F) * 8

    rgb = np.stack([r, g, b], axis=-1).astype(np.uint8)
    return rgb.reshape(height, width, 3)


# ---------------------------------------------------------------------------
# Normal map generation
# ---------------------------------------------------------------------------

def luminance(rgb: np.ndarray) -> np.ndarray:
    """Perceptual luminance, float32 [0,1]."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0


def sobel_normal_map(height_f32: np.ndarray, strength: float = 6.0) -> np.ndarray:
    """
    Compute tangent-space normals from a float32 heightmap using the Sobel operator.
    Returns (H, W, 3) uint8 normal map in OpenGL convention (B toward viewer).
    """
    # Sobel kernels — compute dH/dx and dH/dy
    from scipy.ndimage import convolve
    kx = np.array([[-1, 0, 1], [-2, 0, 2], [-1, 0, 1]], dtype=np.float32) / 8.0
    ky = np.array([[-1, -2, -1], [0, 0, 0], [1, 2, 1]], dtype=np.float32) / 8.0
    dzdx = convolve(height_f32, kx, mode='wrap') * strength
    dzdy = convolve(height_f32, ky, mode='wrap') * strength

    # Surface normal: N = normalize(-dz/dx, -dz/dy, 1)
    nx = -dzdx
    ny = -dzdy
    nz = np.ones_like(nx)
    length = np.sqrt(nx*nx + ny*ny + nz*nz)
    nx /= length; ny /= length; nz /= length

    # Encode to [0,255]: component 0.0 → 128, -1.0 → 0, +1.0 → 255
    r = np.clip((nx * 0.5 + 0.5) * 255, 0, 255).astype(np.uint8)
    g = np.clip((ny * 0.5 + 0.5) * 255, 0, 255).astype(np.uint8)
    b = np.clip((nz * 0.5 + 0.5) * 255, 0, 255).astype(np.uint8)
    return np.stack([r, g, b], axis=-1)


def height_to_alpha(height_f32: np.ndarray) -> np.ndarray:
    """
    Scale height to [0,255] uint8 for the alpha channel.
    Bright areas (high luminance) map to 255 (raised), dark to 0 (flat).
    The PBR parallax path reads alpha: 1=raised, 0=flat.
    We normalise per-tile so detail is preserved even on dark skins.
    """
    lo, hi = height_f32.min(), height_f32.max()
    if hi - lo < 1e-6:
        return np.full(height_f32.shape, 128, dtype=np.uint8)
    norm = (height_f32 - lo) / (hi - lo)
    return (norm * 255).astype(np.uint8)


def generate_normal_map(rgb: np.ndarray, strength: float = 6.0) -> Image.Image:
    """Full pipeline: RGB texture → RGBA normal map PNG."""
    try:
        from scipy.ndimage import convolve as _  # check availability
        height = luminance(rgb)
        normals = sobel_normal_map(height, strength)
        alpha   = height_to_alpha(height)
    except ImportError:
        # scipy not available — fall back to numpy-only central differences
        height = luminance(rgb).astype(np.float32)
        # pad with wrap
        h_pad = np.pad(height, 1, mode='wrap')
        dzdx = (h_pad[1:-1, 2:] - h_pad[1:-1, :-2]) * 0.5 * strength
        dzdy = (h_pad[2:, 1:-1] - h_pad[:-2, 1:-1]) * 0.5 * strength
        nx = -dzdx; ny = -dzdy; nz = np.ones_like(nx)
        length = np.sqrt(nx*nx + ny*ny + nz*nz)
        nx /= length; ny /= length; nz /= length
        r = np.clip((nx * 0.5 + 0.5) * 255, 0, 255).astype(np.uint8)
        g = np.clip((ny * 0.5 + 0.5) * 255, 0, 255).astype(np.uint8)
        b = np.clip((nz * 0.5 + 0.5) * 255, 0, 255).astype(np.uint8)
        normals = np.stack([r, g, b], axis=-1)
        alpha   = height_to_alpha(height)

    rgba = np.dstack([normals, alpha])
    return Image.fromarray(rgba, 'RGBA')


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Generate PBR normal map from a .CAR texture")
    parser.add_argument('car', help='Path to .CAR file')
    parser.add_argument('--strength', type=float, default=6.0,
                        help='Normal map strength (default 6.0; higher = more relief)')
    parser.add_argument('--out', default=None,
                        help='Output PNG path (default: <stem>_normal.png)')
    args = parser.parse_args()

    car_path = args.car
    if not os.path.exists(car_path):
        print(f"Error: {car_path} not found", file=sys.stderr)
        sys.exit(1)

    stem = os.path.splitext(car_path)[0]
    out_path = args.out or f"{stem}_normal.png"

    print(f"Reading {car_path}...")
    rgb = extract_car_texture(car_path)
    print(f"  Texture: {rgb.shape[1]}x{rgb.shape[0]} RGB555")

    print(f"Generating normal map (strength={args.strength})...")
    img = generate_normal_map(rgb, strength=args.strength)

    img.save(out_path)
    print(f"Saved: {out_path}")
    print(f"  Size: {img.size[0]}x{img.size[1]} RGBA")
    print(f"  Alpha channel encodes height - parallax will be auto-enabled at runtime.")


if __name__ == '__main__':
    main()
