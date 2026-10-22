namespace XrGate.Launcher.Core;

public sealed record BluetoothControllerChoice(string Address, string Label)
{
    public static BluetoothControllerChoice None { get; } = new("none", "Not selected");
}
