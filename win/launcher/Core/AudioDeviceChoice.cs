using System.Text;
using System.Text.Json;

namespace XrGate.Launcher.Core;

public sealed record AudioDeviceChoice(string Id, string Name, bool Available = true)
{
    public string Label => Available ? Name : Name + " (not connected / unavailable)";
    public static AudioDeviceChoice None { get; } = new("none", "No audio");
    public static AudioDeviceChoice Default { get; } = new("default", "Default Windows output");
}

public sealed class AudioDeviceCatalog
{
    public static string OutputForStart(bool enabled, string saved, IEnumerable<AudioDeviceChoice> outputs)
    {
        if (!enabled) return saved;
        var available = outputs.Where(item => item.Available && item.Id.StartsWith("alsa:", StringComparison.Ordinal)).ToList();
        return available.Any(item => item.Id == saved) ? saved : available.FirstOrDefault()?.Id ?? saved;
    }

    public List<AudioDeviceChoice> Sources { get; set; } = [];
    public List<AudioDeviceChoice> Outputs { get; set; } = [];
    public static AudioDeviceCatalog Load(string path)
    {
        if (!File.Exists(path)) return new();
        var result = JsonSerializer.Deserialize<AudioDeviceCatalog>(File.ReadAllText(path, Encoding.UTF8))
            ?? throw new InvalidDataException("Invalid saved audio device list.");
        if (result.Sources is null || result.Outputs is null || result.Sources.Concat(result.Outputs).Any(item => item is null ||
            string.IsNullOrEmpty(item.Id) || item.Name is null)) throw new InvalidDataException("Invalid saved audio device list.");
        return result;
    }
    public void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".new";
        try
        {
            File.WriteAllText(temporary, JsonSerializer.Serialize(this), new UTF8Encoding(false));
            File.Move(temporary, path, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    // Selection is an identity, never an index or friendly name. Disappearance
    // preserves a disabled entry instead of selecting another audio device.
    public static List<AudioDeviceChoice> Merge(IEnumerable<AudioDeviceChoice> remembered,
        IEnumerable<AudioDeviceChoice> discovered, string selected, bool source)
    {
        var choices = new Dictionary<string, AudioDeviceChoice>(StringComparer.Ordinal);
        foreach (var item in remembered.Where(item => item.Id is not ("none" or "default")))
            choices[item.Id] = item with { Available = false };
        foreach (var item in discovered.Where(item => item.Id is not ("none" or "default"))) choices[item.Id] = item;
        if (selected.Length > 0 && selected is not ("none" or "default") && !choices.ContainsKey(selected))
            choices[selected] = new(selected, "Saved audio device [" + selected + "]", false);
        var result = new List<AudioDeviceChoice> { AudioDeviceChoice.None };
        if (source) result.Add(AudioDeviceChoice.Default);
        result.AddRange(choices.Values.OrderByDescending(item => item.Available).ThenBy(item => item.Name, StringComparer.OrdinalIgnoreCase));
        return result;
    }
}
