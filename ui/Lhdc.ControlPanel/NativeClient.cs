using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json.Nodes;
using Microsoft.Win32;
using System.Security.AccessControl;
using System.Security.Cryptography;
namespace Lhdc.ControlPanel;
public sealed class AudioModeRestartRequiredException:IOException {
    public AudioModeRestartRequiredException():base("Windows 尚未完成音频驱动切换，需要手动重启电脑后重试。") {}
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
    private readonly string applicationRoot;
    private readonly string powerShellExecutable;
    public NativeClient() {
        applicationRoot=AppContext.BaseDirectory;
        powerShellExecutable=Path.Combine(applicationRoot,"runtime","powershell","pwsh.exe");
        foreach(var relative in new[]{"scripts/Set-AudioMode.ps1","backend/lhdc-host.exe","runtime/powershell/pwsh.exe"}) {
            var path=Path.Combine(applicationRoot,relative);
            if(!File.Exists(path)) throw new FileNotFoundException("发布包不完整，请重新解压后运行。",path);
        }
    }
    public async Task<JsonObject> Host(params string[] arguments) {
        var start=new ProcessStartInfo(Path.Combine(applicationRoot,"backend","lhdc-host.exe")) {
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
        var script=Path.Combine(applicationRoot,"scripts","Set-AudioMode.ps1");
        var adaptive=profile.AdaptiveBitrate?" -AdaptiveBitrate":"";
        var command=$"& {Quote(script)} -PackageRoot {Quote(applicationRoot)} -Mode {Quote(mode)} -SampleRate {profile.SampleRate} -Bits {profile.Bits} -Kbps {profile.Kbps}{adaptive} -ResultFile {Quote(resultFile)}";
        return RunElevated(command,resultFile);
    }
    public bool ServiceBinaryCurrent() {
        var installed=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),"LHDC-Win","lhdc-service.exe");
        if(!File.Exists(installed)) return false;
        using var current=File.OpenRead(installed);
        using var bundled=File.OpenRead(Path.Combine(applicationRoot,"backend","lhdc-service.exe"));
        return SHA256.HashData(current).AsSpan().SequenceEqual(SHA256.HashData(bundled));
    }
    private static string Quote(string value)=>"'"+value.Replace("'","''")+"'";
    private async Task<JsonObject> RunElevated(string command,string resultFile) {
        var start=new ProcessStartInfo(powerShellExecutable) {
            UseShellExecute=true,Verb="runas",WindowStyle=ProcessWindowStyle.Hidden,
            Arguments="-NoProfile -ExecutionPolicy Bypass -EncodedCommand "+Convert.ToBase64String(Encoding.Unicode.GetBytes(command))
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
