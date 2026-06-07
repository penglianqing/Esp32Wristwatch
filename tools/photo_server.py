#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import logging
import random
from pathlib import Path

from flask import Flask, Response, abort, request
from PIL import Image, ImageOps


IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}


def find_images(root: Path) -> list[Path]:
    return [
        path
        for path in root.rglob("*")
        if path.is_file() and path.suffix.lower() in IMAGE_EXTS
    ]


def cover_square(image: Image.Image, size: int) -> Image.Image:
    image = ImageOps.exif_transpose(image).convert("RGB")
    width, height = image.size
    edge = min(width, height)
    left = (width - edge) // 2
    top = (height - edge) // 2
    image = image.crop((left, top, left + edge, top + edge))
    return image.resize((size, size), Image.Resampling.LANCZOS)


def rgb565_bytes(image: Image.Image) -> bytes:
    out = bytearray(image.width * image.height * 2)
    pos = 0
    for r, g, b in image.getdata():
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[pos] = value & 0xFF
        out[pos + 1] = value >> 8
        pos += 2
    return bytes(out)


def jpeg_bytes(image: Image.Image, quality: int) -> bytes:
    output = io.BytesIO()
    image.save(output, format="JPEG", quality=quality, optimize=True, progressive=False)
    return output.getvalue()


def create_app(image_dir: Path, size: int, quality: int) -> Flask:
    app = Flask(__name__)

    @app.get("/image.rgb565")
    def image_rgb565() -> Response:
        images = find_images(image_dir)
        if not images:
            abort(404, f"No images found in {image_dir}")

        path = random.choice(images)
        with Image.open(path) as image:
            frame = cover_square(image, size)
            payload = rgb565_bytes(frame)

        app.logger.info(
            "GET /image.rgb565 from %s -> %s (%d bytes)",
            request.remote_addr,
            path.name,
            len(payload),
        )
        headers = {
            "Cache-Control": "no-store",
            "X-Source-Image": path.name,
        }
        return Response(payload, headers=headers, mimetype="application/octet-stream")

    @app.get("/image.jpg")
    def image_jpg() -> Response:
        images = find_images(image_dir)
        if not images:
            abort(404, f"No images found in {image_dir}")

        path = random.choice(images)
        with Image.open(path) as image:
            frame = cover_square(image, size)
            payload = jpeg_bytes(frame, quality)

        app.logger.info(
            "GET /image.jpg from %s -> %s (%d bytes)",
            request.remote_addr,
            path.name,
            len(payload),
        )
        headers = {
            "Cache-Control": "no-store",
            "X-Source-Image": path.name,
        }
        return Response(payload, headers=headers, mimetype="image/jpeg")

    return app


def main() -> None:
    parser = argparse.ArgumentParser(description="Random RGB565 photo server for ESP32 dashboard")
    parser.add_argument("image_dir", type=Path, help="Directory containing source images")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--size", type=int, default=466)
    parser.add_argument("--quality", type=int, default=85)
    args = parser.parse_args()

    image_dir = args.image_dir.expanduser().resolve()
    if not image_dir.is_dir():
        raise SystemExit(f"Not a directory: {image_dir}")

    logging.basicConfig(level=logging.INFO)
    app = create_app(image_dir, args.size, args.quality)
    app.run(host=args.host, port=args.port)


if __name__ == "__main__":
    main()
