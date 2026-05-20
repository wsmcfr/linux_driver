#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
generate_boot_splash_asset.py

作用：
    把 ai_boot_splash_preview.html 这个设计源渲染成板端可直接写入 framebuffer 的启动图资源。

主要流程：
    1. 使用本机 Microsoft Edge/Chromium 无头模式把 HTML 截成 1024x600 PNG。
    2. 使用 Pillow 读取 PNG，并强制校验尺寸为 1024x600。
    3. 把每个 RGB 像素转换成 STM32MP157 当前 framebuffer 常用的 RGB565 little-endian 原始数据。
    4. 输出 boot_splash.png 和 boot_splash.rgb565，供 fb_boot_splash 在 Qt 启动前直接 blit。

关键说明：
    - HTML 是唯一设计源，后续想调整开机图视觉时优先改 HTML，再运行本脚本重新生成资源。
    - 板端启动阶段不运行浏览器、不解析 PNG、不加载字体库，只读取 RGB565 raw 文件。
    - “AI 开机静态启动图预览”这个标题来自 HTML，用于静态契约确认脚本确实服务当前启动图。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable

from PIL import Image
from playwright.sync_api import sync_playwright


# REF_WIDTH 是板端 RGB LCD 和 HTML 预览稿共同使用的设计宽度。
REF_WIDTH = 1024

# REF_HEIGHT 是板端 RGB LCD 和 HTML 预览稿共同使用的设计高度。
REF_HEIGHT = 600

# EXPECTED_RAW_SIZE 是 RGB565 每像素 2 字节时的精确文件大小，用于防止生成半截资源。
EXPECTED_RAW_SIZE = REF_WIDTH * REF_HEIGHT * 2


def run_html_screenshot(html: Path, png: Path) -> None:
    """
    使用 Playwright/Chromium 把 HTML 的 .boot-frame 元素渲染成 PNG。

    主要流程：
        1. 删除旧 PNG，避免浏览器失败时误用旧输出。
        2. 使用 file:// URL 加载本地 HTML。
        3. 把 viewport 固定为 1024x600，并移除 body padding 带来的预览外框。
        4. 只截取 .boot-frame 元素，确保输出不包含浏览器背景或外部黑边。
        5. 检查输出文件是否存在。

    关键参数：
        html 是 HTML 设计源路径。
        png 是输出 PNG 路径。

    返回值：
        无返回值；失败时抛出 RuntimeError。
    """
    if png.exists():
        png.unlink()

    html_url = html.resolve().as_uri()
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(channel="msedge", headless=True)
        try:
            page = browser.new_page(
                viewport={"width": REF_WIDTH, "height": REF_HEIGHT},
                device_scale_factor=1,
            )
            page.goto(html_url, wait_until="networkidle")
            page.add_style_tag(
                content="""
                html, body {
                    width: 1024px !important;
                    height: 600px !important;
                    padding: 0 !important;
                    margin: 0 !important;
                    overflow: hidden !important;
                    background: transparent !important;
                }

                body {
                    display: block !important;
                }

                .boot-frame {
                    width: 1024px !important;
                    height: 600px !important;
                    aspect-ratio: auto !important;
                    border: 0 !important;
                    box-shadow: none !important;
                }
                """
            )
            frame = page.locator(".boot-frame")
            frame.screenshot(path=str(png.resolve()))
        finally:
            browser.close()

    if not png.is_file():
        raise RuntimeError(f"HTML 截图命令结束但没有生成 PNG：{png}")


def rgb888_to_rgb565_le(pixel: tuple[int, int, int]) -> bytes:
    """
    把一个 RGB888 像素转换为 RGB565 little-endian 字节。

    主要流程：
        1. 红色取高 5 位，绿色取高 6 位，蓝色取高 5 位。
        2. 按 RGB565 位布局组装 16bit 值。
        3. 低字节在前输出，匹配 ARM little-endian framebuffer 写入习惯。

    关键参数：
        pixel 是一个 (r, g, b) 三元组，每个分量范围为 0..255。

    返回值：
        返回长度为 2 的 bytes。
    """
    r, g, b = pixel
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes((value & 0xFF, (value >> 8) & 0xFF))


def iter_rgb565_rows(image: Image.Image) -> Iterable[bytes]:
    """
    按行生成 RGB565 little-endian 数据。

    主要流程：
        1. 把图片转换为 RGB，去掉 alpha，避免半透明 PNG 在板端解析不一致。
        2. 从左到右、从上到下遍历像素。
        3. 每一行拼成一个 bytes 块，减少写文件调用次数。

    关键参数：
        image 是 Pillow 打开的图片对象。

    返回值：
        逐行 yield RGB565 字节串。
    """
    rgb_image = image.convert("RGB")
    for y in range(REF_HEIGHT):
        row = bytearray()
        for x in range(REF_WIDTH):
            row.extend(rgb888_to_rgb565_le(rgb_image.getpixel((x, y))))
        yield bytes(row)


def convert_png_to_rgb565(png: Path, raw: Path) -> None:
    """
    把 1024x600 PNG 转换成 RGB565 raw 文件。

    主要流程：
        1. 打开 PNG 并校验尺寸。
        2. 遍历像素生成 RGB565 little-endian 数据。
        3. 写入临时文件后原子替换，避免中断时留下半截资源。
        4. 校验最终 raw 文件大小必须等于 1024*600*2。

    关键参数：
        png 是输入 PNG 路径。
        raw 是输出 RGB565 raw 路径。

    返回值：
        无返回值；失败时抛出异常。
    """
    with Image.open(png) as image:
        if image.size != (REF_WIDTH, REF_HEIGHT):
            raise RuntimeError(f"PNG 尺寸必须是 {REF_WIDTH}x{REF_HEIGHT}，实际是 {image.size}")

        tmp_raw = raw.with_suffix(raw.suffix + ".tmp")
        with tmp_raw.open("wb") as out:
            for row in iter_rgb565_rows(image):
                out.write(row)

        tmp_raw.replace(raw)

    actual_size = raw.stat().st_size
    if actual_size != EXPECTED_RAW_SIZE:
        raise RuntimeError(f"RGB565 文件大小错误：期望 {EXPECTED_RAW_SIZE}，实际 {actual_size}")


def parse_args() -> argparse.Namespace:
    """
    解析命令行参数。

    主要流程：
        1. 默认把脚本同目录下的 ai_boot_splash_preview.html 作为输入。
        2. 默认输出 boot_splash.png 和 boot_splash.rgb565。
        3. 允许 --skip-render 只从已有 PNG 重新生成 raw，便于离线环境快速验证。

    关键参数：
        无显式参数；读取 sys.argv。

    返回值：
        返回 argparse.Namespace，包含 html/png/raw/skip_render。
    """
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="从 HTML 生成 STM32MP157 framebuffer 启动图资源")
    parser.add_argument("--html", default=str(script_dir / "ai_boot_splash_preview.html"), help="HTML 设计源")
    parser.add_argument("--png", default=str(script_dir / "boot_splash.png"), help="输出 PNG 文件")
    parser.add_argument("--raw", default=str(script_dir / "boot_splash.rgb565"), help="输出 RGB565 raw 文件")
    parser.add_argument("--skip-render", action="store_true", help="跳过 HTML 渲染，只把已有 PNG 转成 RGB565")
    return parser.parse_args()


def main() -> int:
    """
    程序入口。

    主要流程：
        1. 解析输入输出路径。
        2. 必要时调用 Playwright 无头浏览器截图。
        3. 把 PNG 转成 RGB565 raw。
        4. 输出生成结果和 raw 文件大小，供部署前人工确认。

    关键参数：
        无显式参数；读取命令行参数。

    返回值：
        成功返回 0；失败时输出错误并返回 1。
    """
    args = parse_args()
    html = Path(args.html)
    png = Path(args.png)
    raw = Path(args.raw)

    try:
        if not html.is_file():
            raise RuntimeError(f"找不到 HTML 设计源：{html}")

        if not args.skip_render:
            run_html_screenshot(html, png)

        convert_png_to_rgb565(png, raw)
    except Exception as exc:  # noqa: BLE001 - 命令行工具需要把所有失败转换成清楚的 stderr。
        print(f"错误：{exc}", file=sys.stderr)
        return 1

    print(f"PNG: {png} ({REF_WIDTH}x{REF_HEIGHT})")
    print(f"RGB565: {raw} ({raw.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
