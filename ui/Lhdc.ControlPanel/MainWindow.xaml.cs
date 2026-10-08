using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace Lhdc.ControlPanel;

public partial class MainWindow : Window {
    private static readonly int[] QualityBitrates = [64,160,192,256,320,400,500,900,1000];
    private NativeClient? client;
    private JsonArray? rates, bitDepths;
    private string peerCaps = "", service = "", logFile = "";
    private int sampleRate = 48000, bits = 24, preferredKbps = 400;
    private bool initializing = true, busy, refreshing, updatingParameters;
    private bool audioServiceRunning, profileEditable, modePendingConnection;
    private bool formatEdited;
    private bool deviceAvailable;
    private bool headphoneConnected;
    private bool? hiResEnabled;
    private int mediaMtu;
    private readonly DispatcherTimer statusTimer = new() { Interval = TimeSpan.FromSeconds(3) };

    public MainWindow() { InitializeComponent(); }

    private async void OnLoaded(object sender, RoutedEventArgs e) {
        await Operate(async () => {
            Directory.CreateDirectory(ProfileStore.DirectoryPath);
            logFile = Path.Combine(ProfileStore.DirectoryPath, $"panel-{DateTime.Now:yyyyMMdd-HHmmss}-{Environment.ProcessId}.log");
            client = new NativeClient();
            var saved = ProfileStore.Load();
            if (saved.Mode != "lhdc" && saved.Mode != "windows")
                throw new InvalidDataException("保存的音频模式无效");
            ModeBox.SelectedIndex = saved.Mode == "lhdc" ? 0 : 1;
            var installed = NativeClient.InstalledProfile() ?? saved;
            sampleRate = installed.SampleRate;
            bits = installed.Bits;
            preferredKbps = CanonicalBitrate(installed.SampleRate, installed.Kbps);
            UpdateFormats();
            UpdateBitrates();
            await Refresh();
            if (service == "BthA2dp") ModeBox.SelectedIndex = 1;
            else if (service == "lhdc-transport") ModeBox.SelectedIndex = 0;
            initializing = false;
            StatusText.Text = "就绪";
            statusTimer.Tick += async (_, _) => {
                if (busy || refreshing) return;
                refreshing = true;
                try { await Refresh(false); }
                catch (Exception error) { DriverText.Text = "设备状态暂不可用"; Log(error.ToString()); }
                finally { refreshing = false; }
            };
            statusTimer.Start();
        }, "正在读取设备与配置…");
    }

    private static int CanonicalBitrate(int rate, int kbps) => rate == 44100
        ? kbps switch { 240 => 256, 480 => 500, _ => kbps } : kbps;
    private int ActualBitrate(int canonical) => sampleRate == 44100
        ? canonical switch { 256 => 240, 500 => 480, _ => canonical } : canonical;
    private AudioProfile StoredProfile() => new(ModeBox.SelectedIndex == 0 ? "lhdc" : "windows", sampleRate, bits, ActualBitrate(preferredKbps));
    private Task<JsonObject> ValidateProfile() {
        var kbps = ActualBitrate(((Choice)BitrateBox.SelectedItem).Value);
        var arguments = new[] {
            sampleRate.ToString(CultureInfo.InvariantCulture),
            bits.ToString(CultureInfo.InvariantCulture),
            kbps.ToString(CultureInfo.InvariantCulture)
        };
        return peerCaps.Length > 0
            ? client!.Host("configure", peerCaps, arguments[0], arguments[1], arguments[2])
            : client!.Host(["profile-check", ..arguments]);
    }

    private void UpdateBitrates() {
        var rate = rates?.FirstOrDefault(r => r!["sample_rate"]!.GetValue<int>() == sampleRate);
        var allowed = rate?["bitrates"]?.AsArray().Select(b => b!.GetValue<int>()).ToHashSet();
        var available = QualityBitrates.Where(k => allowed is null ? k == preferredKbps : allowed.Contains(ActualBitrate(k)) && (mediaMtu > 0 || k <= 400))
            .Select(k => new Choice(k, ActualBitrate(k) + " kbps")).ToArray();
        updatingParameters = true;
        try {
            BitrateBox.ItemsSource = available;
            BitrateBox.SelectedItem = available.FirstOrDefault(c => c.Value == preferredKbps);
        }
        finally { updatingParameters = false; }
    }
    private void UpdateFormats() {
        var supportedRates = rates?.Select(r => r!["sample_rate"]!.GetValue<int>()).ToArray() ?? [sampleRate];
        var supportedDepths = bitDepths?.Select(b => b!.GetValue<int>()).ToArray() ?? [bits];
        updatingParameters = true;
        try {
            SampleRateBox.ItemsSource = supportedRates.Select(r => new Choice(r, $"{(r / 1000.0).ToString("0.#", CultureInfo.InvariantCulture)} kHz")).ToArray();
            BitDepthBox.ItemsSource = supportedDepths.Select(b => new Choice(b, $"{b}-bit")).ToArray();
            SampleRateBox.SelectedItem = SampleRateBox.Items.Cast<Choice>().FirstOrDefault(c => c.Value == sampleRate);
            BitDepthBox.SelectedItem = BitDepthBox.Items.Cast<Choice>().FirstOrDefault(c => c.Value == bits);
        }
        finally { updatingParameters = false; }
    }
    private void FormatChanged(object sender, SelectionChangedEventArgs e) {
        if (initializing || updatingParameters) return;
        if (SampleRateBox.SelectedItem is not Choice rate || BitDepthBox.SelectedItem is not Choice depth) return;
        sampleRate = rate.Value;
        bits = depth.Value;
        formatEdited = true;
        UpdateBitrates();
        Edited();
    }

    private void ModeChanged(object sender, SelectionChangedEventArgs e) { if (!initializing) Edited(); }
    private void ParameterChanged(object sender, SelectionChangedEventArgs e) {
        if (initializing || updatingParameters) return;
        if (BitrateBox.SelectedItem is Choice choice) preferredKbps = choice.Value;
        Edited();
    }
    private void Edited() {
        UpdateControls();
        if (!busy) StatusText.Text = "设置已修改，等待应用";
    }
    private void UpdateControls() {
        bool ready = !initializing && !busy;
        bool lhdc = ModeBox.SelectedIndex == 0;
        ModeBox.IsEnabled = ready;
        ParameterGrid.IsEnabled = ready;
        ParameterGrid.Visibility = lhdc ? Visibility.Visible : Visibility.Collapsed;
        FormatSelectors.Visibility = lhdc ? Visibility.Visible : Visibility.Collapsed;
        FormatSelectors.IsEnabled = ready;
        FormatText.Visibility = lhdc ? Visibility.Collapsed : Visibility.Visible;
        FormatHint.Text = lhdc ? "格式切换会短暂中断音频。" : "在 Windows 声音设置中调整";
        ModeHint.Visibility = lhdc ? Visibility.Collapsed : Visibility.Visible;
        SaveButton.IsEnabled = ready && deviceAvailable && (!lhdc || BitrateBox.SelectedItem is Choice && SampleRateBox.SelectedItem is Choice && BitDepthBox.SelectedItem is Choice);
        RefreshButton.IsEnabled = ready;
        HiResBox.IsEnabled = ready && headphoneConnected && hiResEnabled.HasValue;
        if (!lhdc) SaveButton.Content = "应用 Windows 驱动";
        else if (service == "lhdc-transport" && audioServiceRunning && profileEditable)
            SaveButton.Content = "应用参数";
        else SaveButton.Content = "安装 LHDC 驱动";
    }

    private async Task Refresh(bool writeLog = true) {
        var discovery = await client!.Host("target");
        deviceAvailable = discovery["found"]!.GetValue<bool>();
        var connection = deviceAvailable
            ? await client.Host("connection", discovery["address"]!.GetValue<string>())
            : new JsonObject { ["connected"] = false, ["known"] = false };
        var inventory = await client.Host("inspect");
        var audio = await client.Host("audio-status");
        if (writeLog) {
            Log(connection.ToJsonString());
            Log(inventory.ToJsonString());
            Log(audio.ToJsonString());
        }
        bool connected = connection["connected"]!.GetValue<bool>();
        bool readHiRes = connected && (writeLog || !headphoneConnected);
        headphoneConnected = connected;
        if (!connected) {
            hiResEnabled = null;
            HiResBox.IsChecked = null;
            HiResHint.Text = "连接耳机后读取";
        }
        else if (readHiRes) {
            try { UpdateHiRes(await client.Host("hires")); }
            catch (Exception error) {
                hiResEnabled = null;
                HiResHint.Text = "未能读取耳机状态，请稍后刷新";
                Log(error.ToString());
            }
        }
        bool known = connection["known"]!.GetValue<bool>();
        ConnectionBadge.Text = connected ? "已连接" : known ? "未连接" : "未发现耳机";
        ConnectionBadge.Foreground = connected ? new SolidColorBrush(Color.FromRgb(21,128,61)) : (Brush)FindResource("Muted");
        ConnectionFrame.Background = new SolidColorBrush(connected ? Color.FromRgb(240,253,244) : Color.FromRgb(241,245,249));
        var target = deviceAvailable ? inventory["devices"]!.AsArray().FirstOrDefault(d =>
            d!["instance_id"]!.GetValue<string>().Equals(discovery["instance"]!.GetValue<string>(), StringComparison.OrdinalIgnoreCase)) : null;
        service = target?["service"]?.GetValue<string>() ?? "";
        int problem = target?["problem"]?.GetValue<int>() ?? -1;
        audioServiceRunning = audio["running"]!.GetValue<bool>();
        profileEditable = audio["profile_editable"]?.GetValue<bool>() == true;
        DriverText.Text = target is null || problem != 0 ? "音频设备暂不可用"
            : service == "BthA2dp" ? "Windows 标准音频"
            : service == "lhdc-transport" && audioServiceRunning ? "LHDC V5" : "LHDC V5 · 服务未运行";

        var caps = audio["peer_capabilities"]?.GetValue<string>() ?? "";
        int mtu = audio["media_mtu"]?.GetValue<int>() ?? 0;
        bool formatChanged = caps != peerCaps || mtu != mediaMtu;
        mediaMtu = mtu;
        if (formatChanged) {
            peerCaps = caps;
            var options = caps.Length > 0 ? await client.Host("profile-options", caps, mediaMtu.ToString(CultureInfo.InvariantCulture)) : null;
            rates = options?["rates"]?.AsArray();
            bitDepths = options?["bits"]?.AsArray();
        }
        if (!formatEdited && NativeClient.InstalledProfile() is AudioProfile selected) {
            formatChanged |= sampleRate != selected.SampleRate || bits != selected.Bits;
            sampleRate = selected.SampleRate;
            bits = selected.Bits;
        }
        var devices = await client.Host("audio-devices");
        var endpoint = devices["devices"]!.AsArray().FirstOrDefault(d => {
            var name = d!["name"]!.GetValue<string>();
            return name.Contains("Enco X4", StringComparison.OrdinalIgnoreCase)
                && d["channels"]!.GetValue<int>() == 2
                && name.Contains("LHDC", StringComparison.OrdinalIgnoreCase) == (service == "lhdc-transport");
        });
        if (endpoint is not null) {
            int rate = endpoint["device_sample_rate"]!.GetValue<int>();
            int depth = endpoint["device_bits"]!.GetValue<int>();
            FormatText.Text = $"{(rate / 1000.0).ToString("0.#", CultureInfo.InvariantCulture)} kHz / {depth}-bit";
        }
        else FormatText.Text = "连接设备后显示";
        if (formatChanged) { UpdateFormats(); UpdateBitrates(); }
        UpdateControls();
    }

    private void UpdateHiRes(JsonObject state) {
        hiResEnabled = state["supported"]?.GetValue<bool>() == true ? state["enabled"]?.GetValue<bool>() : null;
        HiResBox.IsChecked = hiResEnabled;
        HiResHint.Text = hiResEnabled == true ? "已开启，格式与码率按耳机实际能力提供。"
            : hiResEnabled == false ? "开启后可使用更高采样率与码率，切换会短暂中断音频。" : "耳机未报告此功能";
    }

    private async void HiResClick(object sender, RoutedEventArgs e) {
        bool desired = HiResBox.IsChecked == true;
        HiResBox.IsChecked = hiResEnabled == true;
        await Operate(async () => {
            var installed = NativeClient.InstalledProfile();
            if (!desired && installed is not null && (installed.SampleRate > 48000 || installed.Kbps > 400)) {
                StatusText.Text = "关闭 Hi-Res 前，请先应用不高于 48 kHz / 400 kbps 的 LHDC 参数";
                return;
            }
            JsonObject result;
            try { result = await client!.Host("hires", desired ? "on" : "off"); }
            catch {
                hiResEnabled = null;
                HiResBox.IsChecked = null;
                HiResHint.Text = "状态未确认，请刷新后重试";
                throw;
            }
            UpdateHiRes(result);
            Log(result.ToJsonString());
            bool capabilitiesPending = false;
            if (result["changed"]?.GetValue<bool>() == true && audioServiceRunning) {
                var deadline = DateTime.UtcNow.AddSeconds(12);
                JsonObject audio;
                do {
                    await Task.Delay(250);
                    audio = await client.Host("audio-status");
                } while ((audio["peer_capabilities"]?.GetValue<string>() ?? "").Length == 0 && DateTime.UtcNow < deadline);
                capabilitiesPending = (audio["peer_capabilities"]?.GetValue<string>() ?? "").Length == 0;
            }
            await Refresh();
            StatusText.Text = capabilitiesPending ? "耳机开关已确认，等待 LHDC 能力更新" : desired ? "Hi-Res 已开启" : "Hi-Res 已关闭";
        }, "正在切换耳机 Hi-Res…");
    }

    private async void RefreshClick(object sender, RoutedEventArgs e) => await Operate(async () => {
        await Refresh();
        StatusText.Text = "已刷新";
    }, "正在刷新设备…");

    private async void SaveClick(object sender, RoutedEventArgs e) => await Operate(async () => {
        if (ModeBox.SelectedIndex == 1) await ApplyMode("Windows");
        else {
            Log((await ValidateProfile()).ToJsonString());
            var installed = NativeClient.InstalledProfile();
            bool sameFormat = installed?.SampleRate == sampleRate && installed.Bits == bits;
            if (service == "lhdc-transport" && audioServiceRunning && profileEditable && sameFormat)
                NativeClient.ApplyCodecProfile(StoredProfile());
            else await ApplyMode("LHDC");
        }
        formatEdited = false;
        ProfileStore.Save(StoredProfile());
        if (modePendingConnection)
            StatusText.Text = ModeBox.SelectedIndex == 0 ? "LHDC 驱动已选择，等待耳机连接" : "Windows 驱动已选择，等待耳机连接";
        else
            StatusText.Text = ModeBox.SelectedIndex == 0 ? "LHDC 设置已应用" : "Windows 立体声音频已恢复";
    }, "正在应用设置…");

    private async Task ApplyMode(string mode) {
        var resultFile = Path.Combine(ProfileStore.DirectoryPath, $"mode-{DateTime.Now:yyyyMMdd-HHmmss-fff}.json");
        var result = await client!.SetAudioMode(mode, StoredProfile(), resultFile);
        modePendingConnection = result["StereoVerificationPending"]?.GetValue<bool>() == true
            || result["FormatVerificationPending"]?.GetValue<bool>() == true;
        Log(result.ToJsonString());
        await Refresh();
    }

    private async Task Operate(Func<Task> action, string message) {
        if (busy) return;
        busy = true;
        UpdateControls();
        StatusText.Text = message;
        try { await action(); }
        catch (AudioModeRestartRequiredException error) { StatusText.Text = error.Message; Log(error.ToString()); }
        catch (Win32Exception error) when (error.NativeErrorCode == 1223) { StatusText.Text = "已取消操作"; }
        catch (Exception error) { StatusText.Text = "操作未完成，请查看诊断日志"; Log(error.ToString()); }
        finally { busy = false; UpdateControls(); }
    }

    private void Log(string message) {
        var line = $"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}";
        LogBox.AppendText(line);
        LogBox.ScrollToEnd();
        if (logFile.Length > 0) File.AppendAllText(logFile, line, Encoding.UTF8);
    }
    private void ShowPage(bool diagnostics) {
        SettingsView.Visibility = diagnostics ? Visibility.Collapsed : Visibility.Visible;
        DiagnosticsView.Visibility = diagnostics ? Visibility.Visible : Visibility.Collapsed;
        SaveButton.Visibility = diagnostics ? Visibility.Collapsed : Visibility.Visible;
        PageTitle.Text = diagnostics ? "诊断日志" : "音频设置";
        SettingsNav.Style = (Style)FindResource(diagnostics ? "Navigation" : "SelectedNavigation");
        DiagnosticsNav.Style = (Style)FindResource(diagnostics ? "SelectedNavigation" : "Navigation");
    }
    private void ShowSettings(object sender, RoutedEventArgs e) => ShowPage(false);
    private void ShowDiagnostics(object sender, RoutedEventArgs e) => ShowPage(true);
    private void OpenLogsClick(object sender, RoutedEventArgs e) => Process.Start(new ProcessStartInfo(ProfileStore.DirectoryPath) { UseShellExecute = true });
    private void OpenSoundSettingsClick(object sender, RoutedEventArgs e) => Process.Start(new ProcessStartInfo("ms-settings:sound") { UseShellExecute = true });
    private void OnClosing(object? sender, CancelEventArgs e) {
        if (!busy) { statusTimer.Stop(); return; }
        e.Cancel = true;
        StatusText.Text = "正在应用设置，请稍候…";
    }
}
