# Controller Gestures

Controller emulation is the default. Keep index and middle straight, ring and
pinky bent, and the thumb relaxed. Turn the hands inward toward your body for
better recognition; the configured offset turns the virtual hands forward.

Left hand: X/Y. Right hand: A/B. Menu requires both hands.

| Button | Gesture |
| --- | --- |
| Trigger | Bend your index finger. Straighten it to release. |
| Grip | Bend your middle finger. Straighten it to release. |
| X / A | Make a thumb–index circle; keep the other three fingers straight. |
| Y / B | Make a thumb–index circle; bend the other three fingers. |
| Thumbstick click / touchpad press | Open one hand toward yourself, with all five fingers straight and spread. Return to your gameplay pose to release. |
| Menu | Hold both hands open, palms facing away from you, like a stop sign. You should see the backs of your hands. |

**Trigger and Grip may work with only a partial bend.**

Touchpad press is a separate gesture. While showing it, movement, Trigger and
Grip are paused. Return to your gameplay pose before using those controls.
Keep both hands visible for Menu. One hand cannot open Menu.

## Movement

Keep your wrist still and your thumb away from your palm.

| Movement | Thumb gesture |
| --- | --- |
| Forward | Straighten and slightly arch your thumb away from your palm. |
| Backward | Curl the tip toward your palm, leaving the base out. |
| Left / right | Point your thumb in the desired direction. |
| Diagonal | Combine sideways movement with the calibrated forward/back gesture. |
| Stop | Return to neutral or tuck your thumb against your palm. |

The newly recorded backward pose produces straight backward movement. Other
positions and diagonals use the existing calibration.

In wrist mode, keep your thumb out and tilt your wrist in the desired direction.
Tucking your thumb stops movement. After an X/Y or A/B gesture, return to the
released pose before using the gameplay buttons again.

## Simple gestures

Select **Gestures (grab / pinch)** in the launcher, or restart tracking with
`SIMPLE_GESTURES=1` on Linux.

| Button | Gesture |
| --- | --- |
| Trigger | Touch thumb and index fingertips. Separate them to release. |
| Grip | Open one palm toward yourself: all five fingers straight and spread. Turn it away or bend the fingers to release. |
| Grip + Trigger | Close a fist with the thumb folded against the fingers. Open it to release both. |
| X (left) / A (right) | Show V: spread straight index and middle fingers; bend ring and pinky. |
| Y (left) / B (right) | Thumbs-up: extend the thumb above the folded knuckles; bend the other four fingers. |
| Menu | Straighten only the index on each hand and point the two index fingers toward each other. Keep both hands visible. |

No calibration is needed. These gestures work outside an active touchpad session.
While the touchpad is active, a fist clicks only that panel, without Grip or Trigger.

### Virtual touchpad (enabled by default)

Open your palm away from you to activate the panel. Its current position relative
to your head becomes the centre. Move the hand left/right and up/down; diagonals
work. The edge is 30 cm from the centre. Movement beyond it stays at the edge.

Close a fist to press, then reopen away from you to release. The centre stays
fixed. A fist can only click after activation. While the panel is active, this
hand's Trigger, Grip and A/B gestures pause; the other hand stays independent.

Open the palm toward yourself (Grip) or remove the hand from view to exit.
The next activation captures a new centre. Loss of head or hand tracking also
releases the panel.

To disable only the panel, add `SIMPLE_GESTURES_TOUCHPAD=0` to the Linux launch
command. In the Windows launcher's `.env`, use `XR_SIMPLE_GESTURES_TOUCHPAD=0`.
Restart tracking after changing it.
