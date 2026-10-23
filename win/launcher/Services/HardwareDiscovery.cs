using System.Runtime.InteropServices;
using XrGate.Launcher.Core;

namespace XrGate.Launcher.Services;

public static class HardwareDiscovery
{
    // Configuration Manager includes USB devices even before usbipd/WSL setup.
    // It reads the Windows PnP inventory and never binds, attaches or opens USB.
    public static IReadOnlySet<string> ConnectedUsbIds()
    {
        const uint present = 0x100, bufferSmall = 0x1A;
        for (int attempt = 0; attempt < 3; attempt++)
        {
            uint result = CM_Get_Device_ID_List_SizeW(out uint length, null, present);
            if (result != 0 || length > 16_000_000) throw new IOException("Could not read the connected USB device inventory (" + result + ").");
            var buffer = new char[Math.Max(length, 2)];
            result = CM_Get_Device_ID_ListW(null, buffer, (uint)buffer.Length, present);
            if (result == bufferSmall) continue; // A device changed during enumeration.
            if (result != 0) throw new IOException("Could not read the connected USB devices (" + result + ").");
            return HardwarePresetChoice.UsbIdsFromInstances(new string(buffer).Split('\0', StringSplitOptions.RemoveEmptyEntries));
        }
        throw new IOException("USB devices changed during detection; refresh status to retry.");
    }

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern uint CM_Get_Device_ID_List_SizeW(out uint length, string? filter, uint flags);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern uint CM_Get_Device_ID_ListW(string? filter, [Out] char[] buffer, uint length, uint flags);
}
