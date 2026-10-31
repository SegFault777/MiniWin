from itertools import product
from pathlib import Path
from PIL import Image

SRC_DIR = Path(__file__).resolve().parent
OUT_DIR = SRC_DIR / "convert"
DITHER = Image.Dither.NONE   # Image.Dither.NONE, FLOYDSTEINBERG


def hex2rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


SYS_START = ["000000", "800000", "008000", "808000", "000080",
             "800080", "008080", "C0C0C0", "C0DCC0", "A6CAF0"]
SYS_END = ["FFFBF0", "A0A0A4", "808080", "FF0000", "00FF00",
           "FFFF00", "0000FF", "FF00FF", "00FFFF", "FFFFFF"]

LEVELS = [0x00, 0x33, 0x66, 0x99, 0xCC, 0xFF]
CUBE = list(product(LEVELS, repeat=3))

sys_start = [hex2rgb(h) for h in SYS_START]
sys_end = [hex2rgb(h) for h in SYS_END]
reserved = set(sys_start + sys_end)

middle = [c for c in CUBE if c not in reserved]

need = 256 - len(sys_start) - len(sys_end) - len(middle)
used = set(sys_start + sys_end + middle)
grays = [(v, v, v) for v in range(256) if (v, v, v) not in used]
step = (len(grays) - 1) / (need - 1)
fill = [grays[round(i * step)] for i in range(need)]

PALETTE = sys_start + middle + fill + sys_end
assert len(PALETTE) == 256 and len(set(PALETTE)) == 256


def build_palette_image() -> Image.Image:
    flat = []
    for r, g, b in PALETTE:
        flat += [r, g, b]
    pal = Image.new("P", (1, 1))
    pal.putpalette(flat)
    return pal


def convert(img: Image.Image, pal: Image.Image):
    """(결과 이미지, 투명 인덱스 또는 None) 을 반환합니다."""
    rgba = img.convert("RGBA")
    r, g, b, a = rgba.split()
    rgb = Image.merge("RGB", (r, g, b))

    out = rgb.quantize(palette=pal, dither=DITHER)

    if a.getextrema()[0] >= 128:
        return out, None  

    opaque_idx = {p for p, alpha in zip(out.getdata(), a.getdata()) if alpha >= 128}
    free = [i for i in range(256) if i not in opaque_idx]
    if not free:
        print("    [경고] 투명용 빈 인덱스가 없어 투명 처리를 생략합니다.")
        return out, None

    t = free[0]
    mask = a.point(lambda v: 255 if v < 128 else 0)
    out.paste(t, mask=mask)
    return out, t


def main():
    OUT_DIR.mkdir(exist_ok=True)
    pal = build_palette_image()
    files = sorted(SRC_DIR.glob("*.png"))
    if not files:
        print("변환할 .png 파일이 없습니다.")

    for src in files:
        try:
            with Image.open(src) as img:
                img.load()
                src_mode = img.mode
                out, t = convert(img, pal)

            out_path = OUT_DIR / src.name
            if t is not None:
                out.info["transparency"] = t
            out.save(out_path, format="PNG")

            with Image.open(out_path) as chk:
                n_pal = len(chk.getpalette() or []) // 3
                print(f"[완료] {src.name}: 원본 {src_mode} -> 저장 {chk.mode}, "
                      f"팔레트 {n_pal}칸, 투명 인덱스 {t}")
        except Exception as e:
            print(f"[실패] {src.name}: {e}")

    print(f"\n완료. 결과 위치: {OUT_DIR}")
    input("\n엔터를 누르면 창이 닫힙니다...")


if __name__ == "__main__":
    main()