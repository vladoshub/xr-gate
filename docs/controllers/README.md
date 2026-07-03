## Emulating a VR controller via external input

Without this, input will be limited to gestures (pinch, grab).
Bluetooth controllers, USB keyboards, and joysticks can be mapped to runtime controller input.
Also you can use 2 identical Bluetooth controllers.

```bash
cd ~/xr-gate-release/xreal_ultra
devices/xreal_ultra/linux/scripts/override_controller/start_override_controller.sh
```

The config will be saved in ~/.config/xr_tracking/override_controller/default.json If you want to retrain, you can delete default.json for new train

If you use an external controller, you can continue to use controller input when you lose hand tracking (default behavior)

I tested on two identical "vr-park" joystick

<p align="center">
  <img src="docs/media/controllers/VR-PARK.png"
       alt="Bluetooth controller mapping"
       width="720">
</p>
