// The shared contract on Windows: every path, name and fixed value the app, the watcher, the
// elevated helper and the bridge (bridge/win/rbstems_bridge_win.c) agree on. The macOS
// counterpart is app/Sources/Paths.swift. Change them here only.
namespace RBStemsPlus;

public static class Consts
{
    /// Where the release assets are downloaded from (the latest GitHub Release). A test build
    /// (RBSTEMSPLUS_TEST_BUILD, scripts/build-windows.sh --test URL) downloads from a local server.
#if RBSTEMSPLUS_TEST_BUILD
    public static readonly string DownloadBase = typeof(Consts).Assembly
        .GetCustomAttributes(typeof(System.Reflection.AssemblyMetadataAttribute), false).OfType<System.Reflection.AssemblyMetadataAttribute>()
        .First(a => a.Key == "RbspTestBase").Value!;
    public static readonly string BuildNote = " - TEST BUILD, downloads from " + DownloadBase;
#else
    public const string DownloadBase = "https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/";
    public const string BuildNote = "";
#endif
    public const string HomeUrl = "https://github.com/Gabe-LS/rbstemsplus";
    public const string TroubleshootingUrl = HomeUrl + "/blob/main/docs/windows.md";
    public const string IssuesPageUrl = HomeUrl + "/issues";
    public const string IssuesUrl = HomeUrl + "/issues/new";

    /// The Windows release's description and its signature (the macOS one is payload.json).
    public const string ManifestName = "payload-windows.json";
    public const string ManifestSigName = "payload-windows.json.sig";

    /// Our model (the 1.0.0 export), as installed in rekordbox's model folder; payload-windows.json
    /// names the current one, and this one is still recognised.
    public const string OurModel = "0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772";
    public const long OurModelSize = 308_572_524;

    /// Who signs Pioneer's rekordbox, its ONNX Runtime and DirectML on Windows (Authenticode): the
    /// certificate's subject organisation (probe of rekordbox 7.2.14, docs/windows-port/NOTES.md).
    public const string PioneerSigner = "AlphaTheta Corporation";

    /// The bridge's marker text and capability line (bridge/win/rbstems_bridge_win.c).
    public const string BridgeMarker = "rbstems bridge: ";
    public const string BridgeCaps = "rbstems-cache: ";

    /// The chosen location's folder and its marker (the same marker as on macOS).
    public const string LocationFolderName = "RB Stems Plus Cache";
    public const string MarkerName = ".rbstemsplus-cache";
    public const string MarkerFormat = "rbstems-v1";
    public const int MarkerMax = 4096;
    public const int CacheDirMax = 700;

    /// The watcher's Run key value name, and the named objects that keep one copy running.
    public const string WatcherRunName = "RB Stems Plus Watcher";
    public const string AppMutex = @"Local\io.github.rbstemsplus.app";
    public const string WatcherMutex = @"Local\io.github.rbstemsplus.watcher";
    public const string AdminMutex = @"Global\io.github.rbstemsplus.admin";
}

/// The folders and files, per account and machine-wide. Root may be set by the tests (a temporary
/// folder that stands in for the account's known folders and ProgramData).
public sealed class AppPaths
{
    public string Roaming { get; }        // %APPDATA%
    public string Local { get; }          // %LOCALAPPDATA%
    public string ProgramData { get; }    // %ProgramData%

    public AppPaths(string roaming, string local, string programData)
    {
        Roaming = roaming; Local = local; ProgramData = programData;
    }

    /// The account's own folders, from the known folders (never from environment variables).
    public static AppPaths ForAccount() => new(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData, Environment.SpecialFolderOption.DoNotVerify),
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData, Environment.SpecialFolderOption.DoNotVerify),
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData, Environment.SpecialFolderOption.DoNotVerify));

    /// A test root: <root>\AppData\Roaming, <root>\AppData\Local, <root>\ProgramData (the bridge's
    /// test layout under RBSTEMS_TEST_HOME).
    public static AppPaths ForTestRoot(string root) => new(
        Path.Combine(root, "AppData", "Roaming"), Path.Combine(root, "AppData", "Local"), Path.Combine(root, "ProgramData"));

    public static AppPaths Current { get; set; } = ForAccount();

    // rekordbox's settings and the STEMS model it downloads (the same layout as on macOS)
    public string RbSettingsDir => Path.Combine(Roaming, "Pioneer", "rekordbox6");
    public string ModelDir => Path.Combine(RbSettingsDir, "models", "demucs3_model");

    // this account's files
    public string Support => Path.Combine(Roaming, "rbstemsplus");
    public string ChosenFile => Path.Combine(Support, "installed");
    public string DismissedFile => Path.Combine(Support, "dismissed");
    public string RequestFile => Path.Combine(Support, "request");
    public string BusyFile => Path.Combine(Support, "busy");
    public string ConfigFile => Path.Combine(Support, "config.ini");
    public string OriginalsDir => Path.Combine(Support, "originals");
    public string PayloadRoot => Path.Combine(Support, "payload");
    public string ManifestFile => Path.Combine(PayloadRoot, Consts.ManifestName);
    public string ManifestSigFile => ManifestFile + ".sig";
    public string PayloadFloorFile => Path.Combine(Support, "payload-version");
    public string WelcomedFile => Path.Combine(Support, "welcomed");
    public string UpdateDir => Path.Combine(Support, "update");
    public string LastUpdateCheckFile => Path.Combine(Support, "update-checked");
    public string LocationHintFile => Path.Combine(Support, "location-hint");
    public string LogDir => Path.Combine(Local, "rbstemsplus", "Logs");
    public string AppLog => Path.Combine(LogDir, "app.log");
    public string WatcherLog => Path.Combine(LogDir, "watcher.log");
    public string AdminLog => Path.Combine(LogDir, "admin.log");
    public string BridgeLog => Path.Combine(LogDir, "bridge.log");
    public string BridgeStateFile => Path.Combine(LogDir, "bridge-state");
    public string DefaultCacheDir => Path.Combine(Local, "rbstemsplus", "Cache");

    // machine-wide (Stems Cache only): Pioneer's real library and the originals, admin-only
    public string RootDir => Path.Combine(ProgramData, "rbstemsplus");
    public string RootOrtDir => Path.Combine(RootDir, "ort");
    public string RootOrt => Path.Combine(RootOrtDir, "onnxruntime.dll");
    public string PioneerBackupDir(string version) => Path.Combine(RootDir, "pioneer", version);

    /// Where the app is installed for this account (bootstrap.ps1 puts it there).
    public string InstallDir => Path.Combine(Local, "Programs", "RB Stems Plus");
    public string InstalledExe => Path.Combine(InstallDir, "RBStemsPlus.exe");
}
