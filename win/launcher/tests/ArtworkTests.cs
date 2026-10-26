using XrGate.Launcher.Core;

public static class ArtworkTests
{
    private static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }

    public static void Run(string folder)
    {
        string root = Path.Combine(folder, "artwork");
        Directory.CreateDirectory(Path.Combine(root, "hardware"));
        Directory.CreateDirectory(Path.Combine(root, "images"));
        File.WriteAllText(Path.Combine(root, ".env.example"), "XR_HARDWARE_ENV=hardware/alpha.env\n" +
            "XR_CONTROLLER_IMAGE_GESTURES=images/hands.png\nXR_CONTROLLER_IMAGE_GEARVR=images/remote.png\n");
        foreach (string device in new[] { "alpha", "beta" })
            File.WriteAllText(Path.Combine(root, "hardware", device + ".env"), "XR_DEVICE_NAME=" + device + "\nXR_DEVICE_IMAGE=images/" + device + ".png\n");
        foreach (string name in new[] { "alpha", "beta", "hands", "remote", "custom", "custom-hands" })
            File.WriteAllText(Path.Combine(root, "images", name + ".png"), "path fixture");
        string state = Path.Combine(root, "state.json");
        var settings = LauncherSettings.Load(root, state);
        var artwork = HeaderArtwork.Select(root, settings, false, false);
        Check(artwork.Device is null && artwork.Controller is null, "Disconnected/stopped artwork was visible.");
        artwork = HeaderArtwork.Select(root, settings, true, false);
        Check(artwork.Device == Path.Combine(root, "images", "alpha.png") && artwork.Controller is null,
            "Connected hardware artwork or idle controller visibility is incorrect.");
        artwork = HeaderArtwork.Select(root, settings, true, true);
        Check(artwork.Controller == Path.Combine(root, "images", "hands.png"), "Running gestures did not select the configured hands.");
        settings.ControllerMode = "gearvr";
        Check(HeaderArtwork.Select(root, settings, true, true).Controller == Path.Combine(root, "images", "remote.png"),
            "Changing controller type did not select its configured image.");
        settings.ControllerMode = "no";
        Check(HeaderArtwork.Select(root, settings, true, true).Controller is null, "No controllers retained artwork.");
        settings.ControllerMode = "gestures";
        settings.HardwareEnv = "hardware/beta.env";
        settings.ApplyHardwarePreset(root);
        Check(HeaderArtwork.Select(root, settings, true, true).Device == Path.Combine(root, "images", "beta.png"),
            "Changing hardware retained the previous device image.");
        File.WriteAllText(Path.Combine(root, ".env"), "XR_DEVICE_IMAGE=images/custom.png\nXR_CONTROLLER_IMAGE_GESTURES=images/custom-hands.png\n");
        settings = LauncherSettings.Load(root, state);
        settings.HardwareEnv = "hardware/beta.env";
        settings.ApplyHardwarePreset(root);
        artwork = HeaderArtwork.Select(root, settings, true, true);
        Check(artwork.Device == Path.Combine(root, "images", "custom.png") && artwork.Controller == Path.Combine(root, "images", "custom-hands.png"),
            "Changing hardware discarded explicit artwork overrides.");
        string outside = Path.Combine(root, "outside.png");
        File.WriteAllText(outside, "outside fixture");
        foreach (string value in new[] { "", "images/missing.png", "outside.png", "images/../outside.png", "../outside.png", outside, "https://example.com/picture.png" })
            Check(HeaderArtwork.LocalImage(root, value) is null, "Missing or external artwork was accepted: " + value);
        File.WriteAllText(Path.Combine(root, "images", "unsupported.svg"), "<svg/>");
        Check(HeaderArtwork.LocalImage(root, "images/unsupported.svg") is null, "An unsupported WPF image format was accepted.");
    }
}
