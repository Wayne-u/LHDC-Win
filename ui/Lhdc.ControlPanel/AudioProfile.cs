using System.IO;
using System.Text;
using System.Text.Json;
namespace Lhdc.ControlPanel;
public sealed record AudioProfile(string Mode="lhdc",int SampleRate=48000,int Bits=24,int Kbps=400);
public sealed record Choice(int Value,string Label) {
    public override string ToString() => Label;
}
public static class ProfileStore {
    public static readonly string DirectoryPath=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"LHDC-Win");
    public static readonly string SettingsPath=Path.Combine(DirectoryPath,"settings.json");
    public static AudioProfile Load()=>File.Exists(SettingsPath)
        ? JsonSerializer.Deserialize<AudioProfile>(File.ReadAllText(SettingsPath,Encoding.UTF8)) ?? throw new InvalidDataException("配置文件为空")
        : new AudioProfile();
    public static void Save(AudioProfile profile) {
        Directory.CreateDirectory(DirectoryPath);
        var temporary=SettingsPath+".tmp";
        File.WriteAllText(temporary,JsonSerializer.Serialize(profile,new JsonSerializerOptions{WriteIndented=true}),Encoding.UTF8);
        File.Move(temporary,SettingsPath,true);
    }
}
