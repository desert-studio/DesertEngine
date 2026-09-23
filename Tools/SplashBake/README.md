# SplashBake — how `Editor/Resources/Splash/Splash.jpg` is made (macOS only, Swift)

    swift Tools/SplashBake/Crop.swift    <panorama.hdr> <crop.png> <u0> <u1> <horizon-from-top> <EV>
    swift Tools/SplashBake/Compose.swift <crop.png> <Splash.jpg> <cropY 0..1> <variant>

`Crop` cuts a 16:9 frame out of the Poly Haven panorama (`klippad_sunrise_2`, 8k `.hdr`, 100 MB — not
in the repository) and grades it: Reinhard, contrast 1.18, saturation 1.12, 6500 -> 5600 K, vignette
0.55. The committed picture used u0 = 0.26, u1 = 0.60, EV 0.3, horizon at 62 % of the height
(`Editor/Resources/Splash/Splash.LICENSE.txt`). `Compose` lays the unchanging parts over it — wordmark,
glow, bottom gradient, copyright — at 1200x675 pt, written @2x (2400x1350) as JPEG q0.95; `cropY` is the
vertical offset of the cover-crop and `variant` is unused. Everything that changes at run time is drawn
live by the editor (`Editor/Source/Editor/Splash/SplashLayout.hpp`).
