# Controller Gestures

Simple gestures are the default. For controller emulation, keep index and middle straight, ring and
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

No calibration is needed. Pinch and palm Grip also work during an active
touchpad session. In that session, V clicks the panel instead of X/A; a fist does not click.

### Virtual touchpad (enabled by default)

Fully straighten all five fingers and separate thumb and index. Point the open
palm away from your head, like a stop sign, with fingers unfolded at the knuckles.
Keep this pose briefly to activate the panel. Its position relative to your head
then becomes the centre. Move the hand left/right and up/down; diagonals
work. The edge is 10 cm from the centre. Movement beyond it stays at the edge.

Show V to press: spread the straight index and middle fingers, with ring and
pinky bent. Release V to release the click. The centre stays fixed, and moving
a held V still changes direction. V only clicks after activation and sends no
X/A during the session; no replacement gesture is assigned to X/A. A fist does
not click. Pinch sends Trigger and an open palm toward yourself sends Grip,
while the axes keep working. The other hand stays independent.

Show thumbs-up (other four fingers bent) to exit without pressing B/Y. Release
the pose before using thumbs-up as a normal button.
Wrist rotation does not exit the panel, including turning the open palm toward
yourself. The 35-degree orientation limit is checked only when activating.
Loss of hand or head tracking also exits.
The next activation captures a new centre.

To disable only the panel, add `SIMPLE_GESTURES_TOUCHPAD=0` to the Linux launch
command. In the Windows launcher's `.env`, use `XR_SIMPLE_GESTURES_TOUCHPAD=0`.
Restart tracking after changing it.


Simple gestures are the default for new sessions/settings. To explicitly select
controller emulation on Linux, use `SIMPLE_GESTURES=0 EMULATION_CONTROLLER=1`.
Existing saved launcher selections are preserved.

Simple-mode settings in `start_xr_runtime_adapter_shm.sh` (or launch environment):

```bash
SIMPLE_GESTURES=1
SIMPLE_GESTURES_TOUCHPAD_RADIUS_M=0.10
SIMPLE_GESTURES_POSE_YAW_OFFSET_DEGREES=30
```

The radius is in metres (0.01–1.0). Positive yaw turns both output hands outward,
over all existing offsets; set it to 0 to disable. The correction affects the
whole skeleton and virtual controller after recognition, so gesture directions
and touchpad axes remain head-relative. As in controller emulation, it is skipped
on a side using controller IMU orientation and returns when that side switches
back to optical hand orientation. It does not accumulate on reconnect.
In the Windows `.env`, use the same radius/yaw names prefixed with `XR_`.
Restart tracking after changing them.
