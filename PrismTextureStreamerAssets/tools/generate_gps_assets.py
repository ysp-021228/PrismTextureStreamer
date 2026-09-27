from pathlib import Path
import struct

WIDTH = 64
HEIGHT = 2048
MIP_LEVELS = 12
ROOT = Path(__file__).resolve().parents[1]
DDS_PATH = ROOT / "home" / "PrismTextureStreamer" / "gps.dds"
TOBJ_PATH = ROOT / "home" / "PrismTextureStreamer" / "gps.tobj"


def rgb565(r, g, b):
    return ((r * 31 // 255) << 11) | ((g * 63 // 255) << 5) | (b * 31 // 255)


def bc3_block(r, g, b, a=255):
    # One solid BC3 block: alpha endpoints/indices followed by opaque BC1 color.
    alpha = bytes((a, a)) + b"\x00" * 6
    color = struct.pack("<HHH", rgb565(r, g, b), rgb565(r, g, b), 0)
    return alpha + color


def dds_payload():
    payload = bytearray()
    width, height = WIDTH, HEIGHT
    while True:
        blocks_x = max(1, (width + 3) // 4)
        blocks_y = max(1, (height + 3) // 4)
        # A visible four-quadrant pattern makes accidental loading obvious.
        colors = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255))
        for y in range(blocks_y):
            for x in range(blocks_x):
                payload.extend(bc3_block(*colors[(y >= blocks_y // 2) * 2 + (x >= blocks_x // 2)]))
        if width == 1 and height == 1:
            break
        width = max(1, width // 2)
        height = max(1, height // 2)
    return payload


def write_dds():
    # DDS_HEADER + DX10 is avoided: FourCC DXT5 is BC3_UNORM, broadly supported by Prism3D.
    flags = 0x00021007  # CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE | MIPMAPCOUNT
    caps = 0x00400808   # COMPLEX | TEXTURE | MIPMAP
    pixel_format = struct.pack("<II4s5I", 32, 0x00000004, b"DXT5", 0, 0, 0, 0, 0)
    header = struct.pack(
        "<7I11I",
        124, flags, HEIGHT, WIDTH,
        max(16, ((WIDTH + 3) // 4) * ((HEIGHT + 3) // 4) * 16),
        0, MIP_LEVELS, *([0] * 11)
    ) + pixel_format + struct.pack("<5I", caps, 0, 0, 0, 0)
    assert len(header) == 124
    DDS_PATH.parent.mkdir(parents=True, exist_ok=True)
    DDS_PATH.write_bytes(b"DDS " + header + dds_payload())


def write_tobj():
    # SCS TOBJ default-texture header, matching the public TOBJEditor format.
    header = bytes.fromhex(
        "01 0A B1 70 00 00 00 00 00 00 00 00 "
        "00 00 00 00 00 00 00 00 01 00 02 00 "
        "02 00 03 03 03 00 00 00 00 01 00 00 "
        "00 01 00 00 35 00 00 00 00 00 00 00"
    )
    path = b"/home/PrismTextureStreamer/gps.dds"
    TOBJ_PATH.write_bytes(header[:40] + bytes((len(path),)) + header[41:48] + path)


if __name__ == "__main__":
    write_dds()
    write_tobj()
    print(f"generated {DDS_PATH}")
    print(f"generated {TOBJ_PATH}")
