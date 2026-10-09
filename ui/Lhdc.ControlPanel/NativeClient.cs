using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Win32;
using System.Security.AccessControl;
namespace Lhdc.ControlPanel;
public sealed record WorkspaceConfig(string WorkspaceRoot,string PowerShellExecutable);
public sealed class AudioModeRestartRequiredException:IOException {
    public AudioModeRestartRequiredException():base("Windows 驱动已选择，请手动重启电脑完成切换。") {}
}
public sealed class NativeClient {
    public static AudioProfile? InstalledProfile() {
        using var key=Registry.LocalMachine.OpenSubKey(@"SOFTWARE\LHDC-Win");
        if(key?.GetValue("Profile") is not byte[] value) return null;
        if(value.Length!=12) throw new InvalidDataException("已安装的 LHDC 配置长度无效");
        var adaptive=key.GetValue("AdaptiveBitrate");
        if(adaptive is not null && (adaptive is not int flag || flag is < 0 or > 1))
            throw new InvalidDataException("自适应码率配置无效");
        return new AudioProfile("lhdc",BitConverter.ToInt32(value,0),BitConverter.ToInt32(value,4),BitConverter.ToInt32(value,8),adaptive is int enabled && enabled==1);
    }
    public static void ApplyCodecProfile(AudioProfile profile) {
        var installed=InstalledProfile();
        if(installed is null || installed.SampleRate!=profile.SampleRate || installed.Bits!=profile.Bits)
            throw new InvalidOperationException("采样格式切换需要重新启用音频设备");
        using var key=Registry.LocalMachine.OpenSubKey(@"SOFTWARE\LHDC-Win",RegistryKeyPermissionCheck.ReadWriteSubTree,RegistryRights.SetValue)
            ?? throw new IOException("LHDC 配置尚未安装");
        byte[] value=[..BitConverter.GetBytes(profile.SampleRate),..BitConverter.GetBytes(profile.Bits),..BitConverter.GetBytes(profile.Kbps)];
        key.SetValue("Profile",value,RegistryValueKind.Binary);
        key.SetValue("AdaptiveBitrate",profile.AdaptiveBitrate?1:0,RegistryValueKind.DWord);
    }
    public WorkspaceConfig Config { get; }
    public NativeClient() {
        Config=JsonSerializer.Deserialize<WorkspaceConfig>(File.ReadAllText(Path.Combine(AppContext.BaseDirectory,"workspace.json"),Encoding.UTF8))
            ?? throw new InvalidDataException("workspace.json 为空");
    }
    public async Task<JsonObject> Host(params string[] arguments) {
        var start=new ProcessStartInfo(Path.Combine(Config.WorkspaceRoot,"build","host","lhdc-host.exe")) {
            UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,
            StandardOutputEncoding=Encoding.UTF8,StandardErrorEncoding=Encoding.UTF8
        };
        foreach(var argument in arguments) start.ArgumentList.Add(argument);
        using var process=Process.Start(start) ?? throw new IOException("无法启动原生后端");
        var stdout=process.StandardOutput.ReadToEndAsync();
        var stderr=process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        var output=await stdout; var error=await stderr;
        if(process.ExitCode!=0) throw new IOException(error.Trim());
        var lines=output.Split('\n',StringSplitOptions.RemoveEmptyEntries);
        return JsonNode.Parse(lines[^1])?.AsObject() ?? throw new InvalidDataException("后端没有返回 JSON");
    }
    public Task<JsonObject> SetAudioMode(string mode,AudioProfile profile,string resultFile) {
        var script=Path.Combine(Config.WorkspaceRoot,"scripts","Set-AudioMode.ps1");
        var adaptive=profile.AdaptiveBitrate?" -AdaptiveBitrate":"";
        var command=$"& {Quote(script)} -Mode {Quote(mode)} -SampleRate {profile.SampleRate} -Bits {profile.Bits} -Kbps {profile.Kbps}{adaptive} -ResultFile {Quote(resultFile)}";
        return RunElevated(command,resultFile);
    }
    private static string Quote(string value)=>"'"+value.Replace("'","''")+"'";
    private async Task<JsonObject> RunElevated(string command,string resultFile) {
        var start=new ProcessStartInfo(Config.PowerShellExecutable) {
            UseShellExecute=true,Verb="runas",WindowStyle=ProcessWindowStyle.Hidden,
            Arguments="-NoProfile -EncodedCommand "+Convert.ToBase64String(Encoding.Unicode.GetBytes(command))
        };
        using var process=Process.Start(start) ?? throw new IOException("无法启动管理员操作");
        await process.WaitForExitAsync();
        if(!File.Exists(resultFile)) throw new IOException($"管理员操作未返回结果，退出码 {process.ExitCode}");
        var result=JsonNode.Parse(await File.ReadAllTextAsync(resultFile,Encoding.UTF8))?.AsObject()
            ?? throw new InvalidDataException("操作结果为空");
        if(result["Completed"]?.GetValue<bool>()!=true && result["RebootRequired"]?.GetValue<bool>()==true)
            throw new AudioModeRestartRequiredException();
        if(result["Completed"]?.GetValue<bool>()!=true)
            throw new IOException($"操作失败：{result["Error"]} 恢复：{result["RecoveryError"]}。证据：{result["EvidenceDirectory"]}");
        return result;
    }
}
