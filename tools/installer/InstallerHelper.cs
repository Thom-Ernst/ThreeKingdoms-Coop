// Native installer file operations; .NET Framework 4, no injection, remoting or PowerShell.
// Rules are ported from Install-TwProxy.ps1 (marker, never double-rename, closed game)
// and Install-TwMod.ps1 (verify before retiring, delayed antivirus recheck).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

internal static class InstallerHelper {
    static Dictionary<string,string> opts = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
    static string result;
    static readonly string[] owned = { "tw3k_coop.dll", "TW3K-Coop-Control.ps1", "TW3K-Coop-Recovery.cmd" };
    // ★ Shipping is variant-specific; ownership spans both so downgrade/uninstall removes debug clients.
    static readonly string[] payload = PayloadStamp.Hashes.Keys.Where(x => x != "amd_ags_x64_proxy.dll").ToArray();
    // Exact old deploy/archive destinations; no wildcard ownership. data is install-only.
    static readonly string[] legacy = { "injector.exe", "Install-TwProxy.ps1", "Invoke-TwControl.ps1", "Watch-TwInject.ps1", "amd_ags_x64_proxy.dll" };
    static readonly string[] retired = { "coop_4player_ui.pack", "coop_gift_panel_LIVE.pack", "coop_gift_panel.pack", "coop_skip_battle_prompts.pack", "coop_dilemma_pump.pack", "coop_gift_panel_DIAG.pack", "coop_gift_panel_PROBE.pack", "tw3k_coop.pack", "skip_intro_movies.pack", "reveal_starting_map.pack" };
    static string Get(string key) { string value; return opts.TryGetValue(key, out value) ? value : ""; }
    static string At(string root, string path) {
        string full = Path.GetFullPath(Path.Combine(root, path));
        if (!full.StartsWith(Path.GetFullPath(root).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))
            throw new Exception("Path escapes the game folder: " + path);
        // Do not follow junctions into someone else's Steam library or saves.
        for (string p = Path.GetDirectoryName(full); p != null; p = Path.GetDirectoryName(p)) {
            if (Directory.Exists(p) && (File.GetAttributes(p) & FileAttributes.ReparsePoint) != 0)
                throw new Exception("Junction/symlink folders are not supported: " + p);
        }
        if (File.Exists(full) && (File.GetAttributes(full) & FileAttributes.ReparsePoint) != 0)
            throw new Exception("Symlink files are not supported: " + full);
        return full;
    }
    static string Hash(string path) {
        using (var sha = SHA256.Create()) using (var f = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
    }
    static bool Ours(string path) {
        if (!File.Exists(path)) return false;
        // Positive identity: the unique exported marker used by Install-TwProxy.ps1.
        // A read error is an error, never "not ours" (antivirus can deny reads).
        return Encoding.ASCII.GetString(File.ReadAllBytes(path)).Contains("Tw3kCoopProxyMarker");
    }
    static void Verify(string source, string dest) {
        if (Hash(source) != Hash(dest)) throw new Exception("Read-back SHA256 mismatch: " + dest);
    }
    static void Replace(string source, string dest) {
        Directory.CreateDirectory(Path.GetDirectoryName(dest));
        string stage = dest + ".tw3k-new";
        if (File.Exists(stage)) throw new Exception("Unexpected staging file; inspect before retrying: " + stage);
        try {
            File.Copy(source, stage); Verify(source, stage);
            // Same-volume atomic replacement: never delete the working DLL first.
            if (File.Exists(dest)) File.Replace(stage, dest, null); else File.Move(stage, dest);
            Verify(source, dest);
        } finally { if (File.Exists(stage)) File.Delete(stage); }
    }
    static void Closed(string game) {
        foreach (string name in new[] { "Three_Kingdoms", "launcher", "CA_Launcher", "TotalWarLauncher" }) {
            var processes = Process.GetProcessesByName(name);
            try {
                foreach (var process in processes) {
                    // Offline fixtures never touch the live game. Only their sleeping EXEs block
                    // a fixture transaction; normal Setup still refuses EVERY matching process.
                    if (Get("fixture") != "") {
                        string prefix = Path.GetFullPath(Get("fixture")).TrimEnd('\\') + "\\";
                        if (!process.MainModule.FileName.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
                    }
                    throw new Exception("Close Three Kingdoms and the Creative Assembly launcher, then click Retry (" + name + " is running).");
                }
            }
            finally { foreach (var p in processes) p.Dispose(); }
        }
        // The script's exclusive-file check also catches mapped/locked files and access denial.
        using (File.Open(At(game, "Three_Kingdoms.exe"), FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
    }
    static void Game(string game) {
        if (!File.Exists(At(game, "Three_Kingdoms.exe"))) throw new Exception("Choose the folder containing Three_Kingdoms.exe. In Steam: Manage > Browse local files.");
        if (Get("fixture") != "") {
            string prefix = Path.GetFullPath(Get("fixture")).TrimEnd('\\') + "\\";
            if (!Path.GetFullPath(game).StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                throw new Exception("Offline fixture target is outside /FIXTUREROOT.");
        }
        Closed(game);
    }
    // Snapshot all affected files BEFORE any mutation. A complete journal permits retry after a
    // killed installer. Original is copied, not renamed: the active DLL exists throughout.
    sealed class Transaction : IDisposable {
        string game, dir; string[] names; bool committed;
        internal Transaction(string g, IEnumerable<string> paths) {
            game = g; dir = At(game, ".tw3k-coop-transaction"); names = paths.Distinct().ToArray();
            if (Directory.Exists(dir)) throw new Exception("A transaction is pending; rerun Setup to recover it.");
            Directory.CreateDirectory(dir);
            try {
                var records = new List<string>();
                for (int i=0; i<names.Length; i++) {
                    string src = At(game, names[i]);
                    if (Directory.Exists(src)) throw new Exception("A folder occupies an installer file path: " + src);
                    if (File.Exists(src)) {
                        using (File.Open(src, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
                        string backup = Path.Combine(dir, i.ToString()); File.Copy(src, backup); Verify(src, backup);
                        records.Add(names[i] + "|" + Hash(backup));
                    }
                    else records.Add(names[i] + "|absent");
                }
                File.WriteAllLines(Path.Combine(dir, "journal"), records, Encoding.UTF8);
            } catch { Directory.Delete(dir, true); throw; }
        }
        internal void Commit() { File.WriteAllText(Path.Combine(dir, "committed"), "ok"); committed = true; Cleanup(dir); }
        public void Dispose() { if (!committed) Recover(game); }
    }
    static void Cleanup(string dir) {
        // Delete commit marker LAST: interruption during cleanup must never turn a committed
        // install into an incomplete rollback with missing backups.
        foreach (string file in Directory.GetFiles(dir)) if (Path.GetFileName(file) != "committed") File.Delete(file);
        string marker = Path.Combine(dir,"committed"); if (File.Exists(marker)) File.Delete(marker);
        Directory.Delete(dir);
    }
    static void Recover(string game) {
        string dir = At(game, ".tw3k-coop-transaction");
        if (!Directory.Exists(dir)) return;
        string journal = Path.Combine(dir, "journal");
        if (File.Exists(journal) && !File.Exists(Path.Combine(dir, "committed"))) {
            string[] names = File.ReadAllLines(journal, Encoding.UTF8);
            // Restore active DLL first, then its saved original. Never move/delete the sole copy.
            for (int i=0; i<names.Length; i++) {
                string[] record = names[i].Split('|');
                if (record.Length != 2) throw new Exception("Invalid recovery journal; keep backups and inspect: " + dir);
                string dest = At(game, record[0]), backup = Path.Combine(dir, i.ToString());
                if (File.Exists(dest + ".tw3k-new")) File.Delete(dest + ".tw3k-new");
                if (record[1] != "absent") {
                    if (!File.Exists(backup) || Hash(backup) != record[1]) throw new Exception("Recovery backup missing or changed; kept working files and remaining backups: " + dir);
                    Replace(backup, dest);
                } else if (File.Exists(dest)) File.Delete(dest);
            }
        }
        Cleanup(dir);
    }
    static string Install(string game, string source) {
        Game(game); Recover(game);
        string proxy = At(game, "amd_ags_x64.dll"), orig = At(game, "amd_ags_x64_orig.dll");
        string incoming = At(source, "amd_ags_x64_proxy.dll");
        foreach (var entry in PayloadStamp.Hashes) if (Hash(At(source, entry.Key)) != entry.Value)
            throw new Exception("Payload SHA256 mismatch: " + entry.Key);
        if (!Ours(incoming)) throw new Exception("Payload proxy has no Tw3kCoopProxyMarker.");
        foreach (string file in payload) if (!File.Exists(At(source, file)) || new FileInfo(At(source,file)).Length == 0)
            throw new Exception("Missing/empty payload: " + file);
        bool ours = Ours(proxy);
        if (!File.Exists(proxy)) throw new Exception("amd_ags_x64.dll is missing. Verify integrity in Steam before installing.");
        if (new FileInfo(proxy).Length == 0 || (File.Exists(orig) && new FileInfo(orig).Length == 0))
            throw new Exception("Original DLL is empty. Verify integrity in Steam before installing.");
        if (ours && (!File.Exists(orig) || Ours(orig))) throw new Exception("Saved original is missing or is another proxy. Verify integrity in Steam before repairing.");
        if (File.Exists(orig) && Ours(orig)) throw new Exception("Saved original is another proxy. Restore game files through Steam and remove the invalid backup before retrying.");
        string state = ours ? "already ours" : File.Exists(orig) ? "Steam update: current original wins" : "fresh";
        var names = new List<string> { "amd_ags_x64.dll", "amd_ags_x64_orig.dll" }; names.AddRange(owned);
        names.AddRange(retired.Select(x => "data\\" + x));
        names.AddRange(legacy);
        var removed = new List<string>();
        using (var transaction = new Transaction(game, names)) {
            if (!ours) Replace(proxy, orig); // New Steam original wins; old copy remains in rollback snapshot until commit.
            foreach (string file in payload) Replace(At(source, file), At(game, file));
            Replace(incoming, proxy);
            System.Threading.Thread.Sleep(6000); // Install-TwMod.ps1: scanning happens AFTER copy closes.
            Verify(incoming, proxy); if (!Ours(proxy)) throw new Exception("Proxy disappeared or changed after copy; check antivirus Protection history.");
            foreach (string file in payload) Verify(At(source, file), At(game, file));
            if (Get("fail") == "after-copy" && Get("fixture") != "") throw new Exception("Offline injected failure after verified copy.");
            foreach (string file in retired) { string p = At(game, "data\\" + file); if (File.Exists(p)) { File.Delete(p); removed.Add("data\\" + file); } }
            foreach (string file in legacy) { string p = At(game, file); if (File.Exists(p)) { File.Delete(p); removed.Add(file); } }
            foreach (string file in owned.Except(payload)) { string p = At(game, file); if (File.Exists(p)) File.Delete(p); }
            if (Get("fail") == "after-cleanup" && Get("fixture") != "") throw new Exception("Offline injected failure after client cleanup.");
            transaction.Commit();
        }
        string enabled = "Subscribe to tw3k_coop in Steam Workshop. Start the game's launcher, tick tw3k_coop under Mods, then play.";
        string workshop = Get("workshop");
        if (workshop != "") {
            if (!Regex.IsMatch(workshop,"^[0-9]+$")) throw new Exception("Invalid Workshop item id.");
            DirectoryInfo common = Directory.GetParent(game);
            DirectoryInfo apps = common == null ? null : common.Parent;
            if (apps != null && apps.Name.Equals("steamapps",StringComparison.OrdinalIgnoreCase)) {
                string download = Path.Combine(apps.FullName,@"workshop\content\779340",workshop);
                if (Directory.Exists(download)) enabled = "Workshop item " + workshop + " is already downloaded. Start the game's launcher, tick tw3k_coop under Mods, then play.";
            }
        }
        string used = At(game, "used_mods.txt");
        if (File.Exists(used) && Regex.IsMatch(File.ReadAllText(used), "(?im)^\\s*mod\\s+\"tw3k_coop\\.pack\"\\s*;"))
            enabled += " tw3k_coop is already enabled in used_mods.txt; check the Workshop copy remains ticked in the launcher.";
        string recovery = PayloadStamp.Debug ? " For a hidden decision blocking the turn, double-click TW3K-Coop-Recovery.cmd in the game folder." : "";
        string variant = PayloadStamp.Debug ? "Developer build (DEBUG)" : "Player build (RELEASE)";
        return variant + ". Installed and SHA256 verified (" + state + "). " + enabled + "\r\nSteam updated and the mod stopped loading? Re-run this setup to repair it." + recovery + "\r\nLegacy files removed: " + (removed.Count==0 ? "none" : string.Join(", ", removed));
    }
    static string Uninstall(string game) {
        // Uninstall owns only root DLL/clients: never snapshot or delete data packs.
        Game(game); Recover(game);
        string proxy = At(game, "amd_ags_x64.dll"), orig = At(game, "amd_ags_x64_orig.dll");
        bool ours = Ours(proxy);
        if ((ours || !File.Exists(proxy)) && (!File.Exists(orig) || Ours(orig)))
            throw new Exception("Cannot restore the original DLL. Verify integrity through Steam, then retry uninstall.");
        var names = new List<string> { "amd_ags_x64.dll", "amd_ags_x64_orig.dll" }; names.AddRange(owned);
        if (Get("logs") == "yes") names.AddRange(Directory.GetFiles(game, "tw3k_coop_*.log").Select(Path.GetFileName));
        using (var transaction = new Transaction(game,names)) {
            // If Steam already restored its DLL, keep that current original byte-for-byte.
            if (ours || !File.Exists(proxy)) Replace(orig, proxy);
            foreach (string file in names.Skip(1)) { string p=At(game,file); if (File.Exists(p)) File.Delete(p); }
            transaction.Commit();
        }
        return "Original DLL restored/preserved exactly. Mod and helpers removed; saves untouched. Logs " + (Get("logs")=="yes" ? "removed." : "kept.");
    }
    static string Registry(RegistryKey root,string path,string name) {
        using(var key=root.OpenSubKey(path)) return key==null ? "" : Convert.ToString(key.GetValue(name,""));
    }
    // Tokenize Valve KeyValues rather than splitting lines: escaped backslashes, quoted strings,
    // nested objects, comments, modern and legacy libraryfolders.vdf all occur in Steam installs.
    static Dictionary<string,object> Vdf(string path) {
        var tokens = Regex.Matches(File.ReadAllText(path), "//[^\\r\\n]*|\"(?:\\\\.|[^\"\\\\])*\"|[{}]|[^\\s{}\"]+")
            .Cast<Match>().Select(m=>m.Value).Where(t=>!t.StartsWith("//")).ToArray();
        int i=0; return Object(tokens,ref i,false);
    }
    static string Token(string t) { if (!t.StartsWith("\"")) return t; return t.Substring(1,t.Length-2).Replace("\\\\", "\\").Replace("\\\"", "\""); }
    static Dictionary<string,object> Object(string[] t, ref int i,bool nested) {
        var d=new Dictionary<string,object>(StringComparer.OrdinalIgnoreCase);
        while(i<t.Length) {
            if(t[i]=="}") { i++; if(!nested) throw new Exception("Unexpected VDF closing brace"); return d; }
            string k=Token(t[i++]); if(i>=t.Length) throw new Exception("Missing VDF value");
            if(t[i]=="{") { i++; d[k]=Object(t,ref i,true); } else d[k]=Token(t[i++]);
        }
        if(nested) throw new Exception("Unclosed VDF object"); return d;
    }
    static object Value(Dictionary<string,object> d,string k) { object v; return d.TryGetValue(k,out v)?v:null; }
    static void Candidate(List<string> found,string path) {
        if(string.IsNullOrEmpty(path)) return;
        try {
            path=Path.GetFullPath(path).TrimEnd('\\');
            if(Get("fixture")!="" && !path.StartsWith(Path.GetFullPath(Get("fixture")).TrimEnd('\\')+"\\",StringComparison.OrdinalIgnoreCase)) return;
            if(File.Exists(At(path,"Three_Kingdoms.exe")) && !found.Contains(path,StringComparer.OrdinalIgnoreCase)) found.Add(path);
        } catch(Exception ex) { Console.Error.WriteLine("Skipped candidate: "+ex.Message); }
    }
    static List<string> Detect() {
        var found=new List<string>(); var roots=new List<string>();
        if(Get("steam")!="") roots.Add(Get("steam"));
        else {
            roots.Add(Registry(Microsoft.Win32.Registry.CurrentUser,@"Software\Valve\Steam","SteamPath"));
            roots.Add(Registry(Microsoft.Win32.Registry.LocalMachine,@"SOFTWARE\WOW6432Node\Valve\Steam","InstallPath"));
        }
        foreach(string root in roots.Where(x=>x!="").Distinct(StringComparer.OrdinalIgnoreCase)) {
            var libraries=new List<string>{root}; string vdf=Path.Combine(root,@"steamapps\libraryfolders.vdf");
            if(File.Exists(vdf)) {
                var top=Vdf(vdf); var lib=Value(top,"libraryfolders") as Dictionary<string,object>;
                if(lib!=null) foreach(var entry in lib) {
                    int n; if(!int.TryParse(entry.Key,out n)) continue;
                    var obj=entry.Value as Dictionary<string,object>;
                    string path=obj==null?entry.Value as string:Value(obj,"path") as string;
                    if(!string.IsNullOrEmpty(path)) libraries.Add(path);
                }
            }
            foreach(string library in libraries.Distinct(StringComparer.OrdinalIgnoreCase)) {
                if(Get("fixture")!="" && !Path.GetFullPath(library).StartsWith(Path.GetFullPath(Get("fixture")).TrimEnd('\\')+"\\",StringComparison.OrdinalIgnoreCase)) continue;
                string manifest=Path.Combine(library,@"steamapps\appmanifest_779340.acf");
                if(!File.Exists(manifest)) continue;
                var app=Value(Vdf(manifest),"AppState") as Dictionary<string,object>;
                if(app!=null && Convert.ToString(Value(app,"appid"))=="779340") {
                    string install=Value(app,"installdir") as string;
                    if(!string.IsNullOrEmpty(install) && install==Path.GetFileName(install)) Candidate(found,Path.Combine(library,@"steamapps\common",install));
                }
            }
        }
        // Manifest evidence takes precedence over a stale Uninstall\Steam App key.
        if(found.Count!=0) return found;
        string stale=Get("uninstalllocation");
        if(Get("steam")=="") stale=Registry(Microsoft.Win32.Registry.LocalMachine,@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 779340","InstallLocation");
        Candidate(found,stale);
        if(Get("steam")=="") foreach(var drive in DriveInfo.GetDrives().Where(x=>x.IsReady && x.DriveType==DriveType.Fixed))
            foreach(string rel in new[]{@"SteamLibrary\steamapps\common",@"Program Files (x86)\Steam\steamapps\common",@"Program Files\Steam\steamapps\common",@"Steam\steamapps\common",@"Games\Steam\steamapps\common",@"Games\SteamLibrary\steamapps\common"})
                Candidate(found,Path.Combine(drive.RootDirectory.FullName,rel,"Total War THREE KINGDOMS"));
        return found;
    }
    static void Write(string message,List<string> candidates) {
        string text="[result]\r\nmessage="+message.Replace("\r", "").Replace("\n", " | ")+"\r\ncount="+candidates.Count+"\r\n";
        for(int i=0;i<candidates.Count;i++) text+="path"+i+"="+candidates[i]+"\r\n";
        if(result!="") File.WriteAllText(result,text,Encoding.Unicode);
        Console.WriteLine(message);
    }
    static int Main(string[] args) {
        foreach(string arg in args.Skip(1)) { int split=arg.IndexOf('='); if(split>0) opts[arg.Substring(0,split)]=arg.Substring(split+1); }
        result=Get("result");
        try {
            if(args.Length==0) throw new Exception("Expected detect, install, check or uninstall.");
            var candidates=new List<string>(); string message;
            switch(args[0]) {
                case "detect": candidates=Detect(); message="Validated Steam game folders: "+candidates.Count; break;
                case "check": Game(Get("game")); message="Game and launcher closed."; break;
                case "install": message=Install(Get("game"),Get("source")); break;
                case "uninstall": message=Uninstall(Get("game")); break;
                default: throw new Exception("Unknown operation.");
            }
            Write(message,candidates); return 0;
        } catch(Exception ex) { Write("Refused: "+ex.Message,new List<string>()); return 1; }
    }
}
