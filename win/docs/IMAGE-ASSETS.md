# Launcher artwork

The original header PNG illustrations in `images/` were generated for XR Gate with the built-in OpenAI image-generation tool. `xreal_ultra.png` is a photorealistic redraw based on the user-supplied reference photograph `1276.jpg`; the source photograph is not bundled. The Gear VR controller and gesture illustrations were generated from text prompts. The original header illustrations do not use downloaded manufacturer photographs or brand logos. These are UI illustrations, not official product photographs.

These project-provided assets are distributed under XR Gate's MIT License, to the extent applicable. Product names and product designs identify the corresponding devices and remain associated with their respective owners; the illustrations do not imply affiliation or endorsement.

Configure the device picture with `XR_DEVICE_IMAGE` in a hardware preset. Configure controller pictures with `XR_CONTROLLER_IMAGE_GESTURES` and `XR_CONTROLLER_IMAGE_GEARVR`. Paths start with `images/`; an empty value hides that picture. Missing or unreadable optional pictures leave tracking available.

## Prompts

### xreal_ultra.png

Use case: product-mockup / precise-object-edit.
Asset type: XREAL Air 2 Ultra hardware illustration for the XR Gate desktop launcher.
Input image: the supplied photograph 1276.jpg is the exact physical product reference. Redraw ONLY these glasses as a clean, photorealistic studio product render on a genuinely transparent background. This is a faithful redraw, not a new design.
Preserve the recognizable construction visible in the photo: broad matte-black brow/top crossbar, squared front outer ends with exactly two small circular forward-facing tracking cameras, one at each outer corner; separate rounded trapezoidal gray-blue tinted lenses with very thin titanium/silver lower and side rims; central black bridge; thick black electronic temples extended backwards with gently bent inner ear tips; small metallic hinge sections. The front top is BLACK, not a silver frame. Keep the optical layers visible subtly through the lenses. Preserve correct proportions, symmetric lenses and realistic camera placement.
Composition: a single complete pair, nearly frontal view from slightly above like the source photo, both open temples visible receding behind the lenses; horizontal landscape composition, entire glasses centered occupying approximately 85% of canvas width with comfortable transparent margins, no clipping. Clean product silhouette readable at small UI sizes.
Style: realistic premium product rendering based closely on this personal photograph, precise edges and believable materials, soft neutral studio lighting and restrained rim highlights so black parts remain legible on a dark UI. Remove dust, scratches, harsh room reflections and sun glare.
Background: true alpha transparency throughout all empty spaces, including between temples and lenses. No desk, printer, wall, paper, shadows on a ground plane, cable, hands, packaging, text, labels or watermark. Do not copy the surrounding photographic scene. Do not invent extra cameras or a VR headband. No generic ski visor or bulky headset.

### controllers_gearvr.png

Use case: product-mockup. Create one premium, accurate standalone illustration of the actual Samsung Gear VR 2017 Bluetooth controller, the small black handheld remote (ET-YO324 family), for a dark desktop application's controller status header. Recognizable real hardware: rounded circular upper head with a large concave circular touchpad on its front, small back and home controls below, a small volume rocker below these, a slightly curved tapered grip with rounded bottom, and a physical trigger on the rear under the upper head. Show the entire controller from a front three-quarter angle so the circular pad, small front controls, and grip are readable. Do not depict an Oculus/Quest/Index/Vive controller, tracking rings, thumbsticks, or a gamepad. Original studio 3D-style rendering, realistic matte charcoal black plastic, soft neutral highlights and subtle cool rim light, clean detailed silhouette, generous padding. Transparent background, no surface or external cast shadow, no hands, no lanyard, no text, brand logos or watermark.

### controllers_gestures.png

Use case: stylized-concept. Create a premium polished 3D-style illustration for a desktop XR hand-gesture controller mode, matching restrained realistic studio product imagery on a dark navy interface. Show two anatomically correct human hands side by side, palms towards the viewer, fingers naturally spread in a relaxed open-palm tracking pose, wrists cropped cleanly just below palms. Exactly five digits per hand, believable joints, opposable thumbs pointing outward, slightly angled complementary orientation. Neutral light warm skin material with subtle cool studio rim lighting, not gloves, not robotic, not wireframe. Entire hands visible with generous padding, silhouettes readable at icon size. Transparent background, no surface, no external shadows, no labels, text, logos, controller devices or watermark. Original elegant realistic illustration.

## MIT License

```text
MIT License

Copyright (c) 2026 XR Tracking contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```


## Illustrated input guides

The SVG hand diagrams in `GESTURES.html` and `GESTURES.ru.html` are original
XR Gate illustrations generated by `tools/build_input_guides.py` and distributed
under the project MIT License. No user photographs are included.

`images/gearvr-controller-model.png` and `images/gearvr-controller-side.png`
are static renders of the Samsung Gear VR model from WebXR Input Profiles:
- Source: https://github.com/immersive-web/webxr-input-profiles/tree/main/packages/assets/profiles/samsung-gearvr
- File: `none.glb`, Git blob `eb087c5fb4374cd9affca46e948d9a6a3474ae31`.
- Copyright (c) 2019 Amazon.
- License: MIT, `packages/assets/LICENSE.md` in that repository.
- Included license: `docs/licenses/WebXR-Input-Profiles/LICENSE.txt` in the Windows bundle.
- Changes: orthographic rendering, lighting and guide annotations.
- Reproduce: `python3 tools/render_gearvr_model.py /path/to/none.glb images/gearvr-controller-model.png`
  (add `--side` for `images/gearvr-controller-side.png`). Rendering requires NumPy and Pillow;
  the launcher and documentation have no such dependency.

The same rendered model is used for LEFT and RIGHT. Arrows describe the default
XR Gate bindings; they are not part of the upstream model.
