"""Regenerates the Shell icons: a pixel-art spiral seashell (16x16), plus the tray states.

Usage (from anywhere): python shell/branding/deploy.py
The pictures are the text grids below; pixelart.py turns them into crisp PNG, ICO and SVG files.
Upstream file names are kept (shell.ico, images/shell-*.ico, logo-shell-*.png) so the code
that loads them is unchanged. Hermit (the client) uses the same shell carried by a hermit crab.
"""
import os

import pixelart as px

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
IMAGES = os.path.join(REPO, 'src_assets', 'common', 'assets', 'web', 'public', 'images')

# Spiral seashell, light from the upper left, cream lip at the lower left
SHELL = px.parse("""
................
.....KKKKKK.....
...KKSSSTTTKK...
..KSSTTTTTTTtK..
.KSTTTKKKKTTttK.
.KSTTKTTTTKTttK.
KSTTKTKKKTKTtttK
KSTTKTKSKTKTtttK
KTTTKTTKTTKTtttK
KTTTTKTTTKTTtttK
KTTTTTKKKTTttttK
.KWTTTTTTTttttK.
.KWWTTTTtttttK..
..KWWKtttttKK...
...KKKKKKKK.....
................
""")

# Tray states: streaming adds a coral live dot, paused is amber, locked is slate
LIVE_DOT = ['..KKK.', '.KRRRK', '.KRwRK', '.KRRRK', '..KKK.']
STATES = {
    'playing': px.overlay(SHELL, LIVE_DOT, 10, 10),
    'pausing': px.recolor(SHELL, {'T': 'A', 't': 'a', 'S': 'Y'}),
    'locked': px.recolor(SHELL, {'T': 'G', 't': 'g', 'S': 'L'}),
}


def write(path, data):
    mode = 'w' if isinstance(data, str) else 'wb'
    with open(path, mode, **({'encoding': 'utf-8', 'newline': '\n'} if mode == 'w' else {})) as f:
        f.write(data)


def main():
    # Sources for reference (and the web logo)
    write(os.path.join(HERE, 'shell.svg'), px.svg(SHELL))
    for state, grid in STATES.items():
        write(os.path.join(HERE, 'shell-%s.svg' % state), px.svg(grid))

    # Exe/installer icon, favicon and web logo
    write(os.path.join(REPO, 'shell.ico'), px.ico(SHELL))
    write(os.path.join(IMAGES, 'shell.ico'), px.ico(SHELL))
    write(os.path.join(IMAGES, 'logo-shell.svg'), px.svg(SHELL))
    write(os.path.join(IMAGES, 'logo-shell-16.png'), px.png(SHELL, 16))
    # Shown at 24 and 45 CSS px; 96 px keeps whole pixels on high-DPI screens
    write(os.path.join(IMAGES, 'logo-shell-45.png'), px.png(SHELL, 96))

    # Tray states
    for state, grid in STATES.items():
        write(os.path.join(IMAGES, 'shell-%s.ico' % state), px.ico(grid))
        write(os.path.join(IMAGES, 'shell-%s.svg' % state), px.svg(grid))
        write(os.path.join(IMAGES, 'shell-%s-16.png' % state), px.png(grid, 16))
        write(os.path.join(IMAGES, 'shell-%s-45.png' % state), px.png(grid, 96))
        write(os.path.join(IMAGES, 'shell-%s.png' % state), px.png(grid, 1024))
    print('Shell icons regenerated')


if __name__ == '__main__':
    main()
