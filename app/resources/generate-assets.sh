#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
RESOURCES="$ROOT/resources"
command -v magick >/dev/null

magick -background none "$RESOURCES/icon.svg" -alpha remove "$RESOURCES/icon.png"
magick -background none "$RESOURCES/icon-foreground.svg" "$RESOURCES/icon-foreground.png"

for entry in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
  density="${entry%%:*}"
  size="${entry##*:}"
  magick "$RESOURCES/icon.png" -resize "${size}x${size}" "$ROOT/android/app/src/main/res/mipmap-$density/ic_launcher.png"
  magick "$RESOURCES/icon.png" -resize "${size}x${size}" "$ROOT/android/app/src/main/res/mipmap-$density/ic_launcher_round.png"
done

for entry in mdpi:108 hdpi:162 xhdpi:216 xxhdpi:324 xxxhdpi:432; do
  density="${entry%%:*}"
  size="${entry##*:}"
  magick "$RESOURCES/icon-foreground.png" -resize "${size}x${size}" "$ROOT/android/app/src/main/res/mipmap-$density/ic_launcher_foreground.png"
done

magick "$RESOURCES/icon.png" -resize 1024x1024 -alpha remove "$ROOT/ios/App/App/Assets.xcassets/AppIcon.appiconset/AppIcon-512@2x.png"
magick "$RESOURCES/icon.png" -resize 64x64 "$ROOT/src/assets/icon/favicon.png"

while IFS= read -r -d '' splash; do
  dimensions="$(magick identify -format '%wx%h' "$splash")"
  width="${dimensions%x*}"
  height="${dimensions#*x}"
  logo=$(( (width < height ? width : height) / 4 ))
  magick -size "$dimensions" xc:'#08110f' \
    \( "$RESOURCES/icon-foreground.png" -resize "${logo}x${logo}" \) \
    -gravity center -composite "$splash"
done < <(find "$ROOT/android/app/src/main/res" "$ROOT/ios/App/App/Assets.xcassets/Splash.imageset" -name '*splash*.png' -print0)

printf 'AntiStalker icons and splash screens generated.\n'
