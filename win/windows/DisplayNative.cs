using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace XrGateWsl {
    public sealed class DesktopDisplay {
        public string DeviceName;
        public string DeviceId;
        public string Name;
        public int X, Y, Width, Height, Frequency;
        public bool Primary;
    }

    public static class DisplayNative {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct DisplayDevice {
            public uint cb;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
            public uint StateFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceId;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
        }

        // DEVMODEW's display union occupies 16 bytes after dmFields.
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct DisplayMode {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
            public ushort SpecVersion, DriverVersion, Size, DriverExtra;
            public uint Fields;
            public int X, Y;
            public uint Orientation, FixedOutput;
            public short Color, Duplex, YResolution, TTOption, Collate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
            public ushort LogPixels;
            public uint BitsPerPel, Width, Height, DisplayFlags, Frequency;
            public uint ICMMethod, ICMIntent, MediaType, DitherType, Reserved1, Reserved2;
            public uint PanningWidth, PanningHeight;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "EnumDisplayDevicesW")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumDisplayDevices(string device, uint index, ref DisplayDevice info, uint flags);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "EnumDisplaySettingsW")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumDisplaySettings(string device, int mode, ref DisplayMode info);

        public static int DeviceStructSize { get { return Marshal.SizeOf(typeof(DisplayDevice)); } }
        public static int ModeStructSize { get { return Marshal.SizeOf(typeof(DisplayMode)); } }

        public static DesktopDisplay[] Enumerate() {
            var result = new List<DesktopDisplay>();
            for (uint i = 0; ; i++) {
                var adapter = new DisplayDevice();
                adapter.cb = (uint)DeviceStructSize;
                if (!EnumDisplayDevices(null, i, ref adapter, 0)) break;
                if ((adapter.StateFlags & 1) == 0 || (adapter.StateFlags & 8) != 0) continue;
                var mode = new DisplayMode();
                mode.Size = (ushort)ModeStructSize;
                if (!EnumDisplaySettings(adapter.DeviceName, -1, ref mode)) continue;
                for (uint j = 0; ; j++) {
                    var monitor = new DisplayDevice();
                    monitor.cb = (uint)DeviceStructSize;
                    if (!EnumDisplayDevices(adapter.DeviceName, j, ref monitor, 1)) break;
                    if ((monitor.StateFlags & 1) == 0 || String.IsNullOrEmpty(monitor.DeviceId)) continue;
                    result.Add(new DesktopDisplay {
                        DeviceName = adapter.DeviceName, DeviceId = monitor.DeviceId,
                        Name = monitor.DeviceString, X = mode.X, Y = mode.Y,
                        Width = (int)mode.Width, Height = (int)mode.Height,
                        Frequency = (int)mode.Frequency, Primary = (adapter.StateFlags & 4) != 0
                    });
                }
            }
            return result.ToArray();
        }
    }
}
