// The Windows window of RunnerBuilder: asks for the game folder, then runs pc/tools/builder.py.
// C# 5, so that the csc.exe that comes with Windows compiles it (make_exe.py). No build logic here.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("RunnerBuilder")]
[assembly: AssemblyVersion("0.2.5.0")]

static class Program {
    [STAThread]
    static void Main(string[] args) {
        ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new BuilderForm(args));
    }
}

sealed class BuilderForm : Form {
    const int VersionCode = 1;
    const string VersionName = "0.2.5";
    const string UpdateUrl = "https://raw.githubusercontent.com/singlemate/RunnerRecompiled/main/version-builder.json";
    const string ProjectUrl = "https://github.com/singlemate/RunnerRecompiled";
    const string TermsUrl = "https://developer.android.com/studio/terms";
    const string PythonUrl = "https://www.python.org/ftp/python/3.13.1/python-3.13.1-embed-amd64.zip";
    const string PythonSha256 = "7b7923ff0183a8b8fca90f6047184b419b108cb437f75fc1c002f9d2f8bcec16";

    readonly TextBox game = new TextBox(), output = new TextBox(), log = new TextBox();
    readonly CheckBox terms = new CheckBox();
    readonly Button start = new Button(), openOut = new Button();
    readonly Label status = new Label();
    readonly ProgressBar bar = new ProgressBar();
    Process running;
    string doneFolder;
    // For tests: RunnerBuilder.exe --auto <game> <out> <log> presses Build and leaves when it ends.
    string autoLog;

    static string Home {
        get {
            // aapt2 cannot open a path with non-ASCII letters, and user names often have them.
            string usual = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RunnerBuilder");
            if (Regex.IsMatch(usual, "^[\\x20-\\x7E]+$")) return usual;
            return Path.Combine(Path.GetPathRoot(Environment.SystemDirectory), "RunnerBuilder");
        }
    }

    public BuilderForm(string[] args) {
        Text = "RunnerBuilder " + VersionName;
        ClientSize = new Size(720, 520);
        MinimumSize = new Size(600, 440);
        Font = new Font("Segoe UI", 9.5f);
        AutoScaleMode = AutoScaleMode.Dpi;
        StartPosition = FormStartPosition.CenterScreen;

        var table = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(12), ColumnCount = 3, RowCount = 9 };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        for (int i = 0; i < 9; i++) table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        table.RowStyles[7] = new RowStyle(SizeType.Percent, 100);

        var intro = new Label {
            AutoSize = true, Margin = new Padding(0, 0, 0, 10), MaximumSize = new Size(690, 0),
            Text = "Программа соберёт приложение для Android из вашей лицензионной копии Mirror's Edge из GOG " +
                   "и подготовит папку, которую останется скопировать на планшет."
        };
        table.Controls.Add(intro, 0, 0); table.SetColumnSpan(intro, 3);

        AddPathRow(table, 1, "Папка игры:", game, "Выберите папку, куда установлена игра из GOG");
        AddPathRow(table, 2, "Сохранить в:", output, "Выберите, куда положить готовые файлы");
        // Not Documents: it is often synced to OneDrive, and the result is about 12 GB of game files.
        output.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "RunnerRecompiled");

        terms.Text = "Принимаю условия Android SDK";
        terms.AutoSize = true;
        terms.Margin = new Padding(0, 8, 0, 0);
        table.Controls.Add(terms, 0, 3); table.SetColumnSpan(terms, 2);
        var link = new LinkLabel { Text = "Прочитать", AutoSize = true, Margin = new Padding(0, 10, 0, 0) };
        link.LinkClicked += delegate { Process.Start(TermsUrl); };
        table.Controls.Add(link, 2, 3);

        var note = new Label {
            AutoSize = true, Margin = new Padding(0, 8, 0, 8), MaximumSize = new Size(690, 0),
            Text = "Понадобится около 25 ГБ на диске и 1,3 ГБ интернета. Результат содержит данные вашей копии игры: не публикуйте и не передавайте его."
        };
        table.Controls.Add(note, 0, 4); table.SetColumnSpan(note, 3);

        start.Text = "Собрать"; start.AutoSize = true; start.Padding = new Padding(16, 2, 16, 2);
        start.Click += delegate { StartBuild(); };
        table.Controls.Add(start, 0, 5);
        status.AutoSize = true; status.Anchor = AnchorStyles.Left; status.Text = "";
        table.Controls.Add(status, 1, 5); table.SetColumnSpan(status, 2);

        bar.Dock = DockStyle.Fill; bar.Height = 14; bar.Margin = new Padding(0, 8, 0, 8);
        table.Controls.Add(bar, 0, 6); table.SetColumnSpan(bar, 3);

        log.Multiline = true; log.ReadOnly = true; log.ScrollBars = ScrollBars.Vertical; log.Dock = DockStyle.Fill;
        log.BackColor = SystemColors.Window; log.Font = new Font("Consolas", 9f);
        table.Controls.Add(log, 0, 7); table.SetColumnSpan(log, 3);

        openOut.Text = "Открыть папку с результатом"; openOut.AutoSize = true; openOut.Visible = false;
        openOut.Margin = new Padding(0, 8, 0, 0);
        openOut.Click += delegate { Process.Start("explorer.exe", "\"" + doneFolder + "\""); };
        table.Controls.Add(openOut, 0, 8); table.SetColumnSpan(openOut, 3);

        Controls.Add(table);
        Shown += delegate { new Thread(CheckForUpdate) { IsBackground = true }.Start(); };
        if (args.Length == 4 && args[0] == "--auto") {
            game.Text = args[1]; output.Text = args[2]; autoLog = args[3]; terms.Checked = true;
            Shown += delegate { StartBuild(); };
        }
        FormClosing += OnClosing;
    }

    void AddPathRow(TableLayoutPanel table, int row, string caption, TextBox box, string prompt) {
        table.Controls.Add(new Label { Text = caption, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 6, 8, 0) }, 0, row);
        box.Dock = DockStyle.Fill; box.Margin = new Padding(0, 3, 6, 3);
        table.Controls.Add(box, 1, row);
        var browse = new Button { Text = "Обзор…", AutoSize = true };
        browse.Click += delegate {
            using (var d = new FolderBrowserDialog { Description = prompt, SelectedPath = box.Text })
                if (d.ShowDialog(this) == DialogResult.OK) box.Text = d.SelectedPath;
        };
        table.Controls.Add(browse, 2, row);
    }

    // One notice at start; any failure is silent, this is only a hint.
    void CheckForUpdate() {
        try {
            string json;
            using (var web = new WebClient()) { web.Encoding = Encoding.UTF8; json = web.DownloadString(UpdateUrl); }
            int code = int.Parse(Regex.Match(json, "\"versionCode\"\\s*:\\s*(\\d+)").Groups[1].Value);
            if (code <= VersionCode) return;
            string name = Regex.Match(json, "\"versionName\"\\s*:\\s*\"([^\"]*)\"").Groups[1].Value;
            string notes = Regex.Match(json, "\"notes\"\\s*:\\s*\"([^\"]*)\"").Groups[1].Value;
            BeginInvoke((Action)delegate {
                string text = "Вышла версия " + name + " (у вас " + VersionName + ")." +
                              (notes.Length > 0 ? "\n\n" + notes : "") + "\n\nОткрыть страницу проекта?";
                if (MessageBox.Show(this, text, "Доступно обновление", MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
                    Process.Start(ProjectUrl);
            });
        } catch (Exception) { }
    }

    void StartBuild() {
        string g = game.Text.Trim(), o = output.Text.Trim();
        if (!File.Exists(Path.Combine(g, "Binaries", "MirrorsEdge.exe"))) {
            MessageBox.Show(this, "В этой папке нет Binaries\\MirrorsEdge.exe. Укажите папку, куда установлена игра из GOG.", Text);
            return;
        }
        if (o.Length == 0) { MessageBox.Show(this, "Укажите, куда сохранить результат.", Text); return; }
        if (!terms.Checked) { MessageBox.Show(this, "Для скачивания инструментов Google нужно принять условия Android SDK.", Text); return; }
        start.Enabled = false; openOut.Visible = false; log.Clear(); doneFolder = null;
        bar.Style = ProgressBarStyle.Marquee;
        new Thread(() => Build(g, o)) { IsBackground = true }.Start();
    }

    void Build(string g, string o) {
        try {
            string python = EnsurePython();
            SetStatus("Подготовка");
            string app = UnpackPayload();
            var info = new ProcessStartInfo(python) {
                Arguments = "-I -X utf8 -u " + Quote(Path.Combine(app, "pc", "tools", "builder.py")) +
                            " --game " + Quote(g) + " --out " + Quote(o) + " --home " + Quote(Home) + " --accept-android-terms",
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
            };
            using (var p = Process.Start(info)) {
                running = p;
                p.OutputDataReceived += (s, e) => { if (e.Data != null) Line(e.Data); };
                p.ErrorDataReceived += (s, e) => { if (e.Data != null) Line(e.Data); };
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                p.WaitForExit();
                running = null;
                Finish(p.ExitCode == 0, "Код завершения " + p.ExitCode);
            }
        } catch (Exception e) {
            Line(e.ToString());
            Finish(false, e.Message);
        }
    }

    string EnsurePython() {
        string dir = Path.Combine(Home, "python"), exe = Path.Combine(dir, "python.exe");
        if (File.Exists(exe)) return exe;
        SetStatus("Скачивание Python");
        Directory.CreateDirectory(Home);
        string zip = Path.Combine(Home, "python.zip");
        using (var web = new WebClient()) web.DownloadFile(PythonUrl, zip);
        using (var sha = SHA256.Create()) using (var f = File.OpenRead(zip)) {
            if (BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "").ToLowerInvariant() != PythonSha256)
                throw new IOException("Python скачался с другой контрольной суммой.");
        }
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        ZipFile.ExtractToDirectory(zip, dir);
        File.Delete(zip);
        return exe;
    }

    // The sources are unpacked anew for every version of the program.
    static string UnpackPayload() {
        string app = Path.Combine(Home, "app-" + VersionName);
        if (Directory.Exists(app)) Directory.Delete(app, true);
        Directory.CreateDirectory(Home);
        string zip = Path.Combine(Home, "payload.zip");
        using (var res = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
        using (var f = File.Create(zip)) res.CopyTo(f);
        ZipFile.ExtractToDirectory(zip, app);
        File.Delete(zip);
        return app;
    }

    static string Quote(string s) {
        // Rules of CommandLineToArgvW: double the backslashes before a closing quote.
        return "\"" + Regex.Replace(s, "(\\\\*)(\"|$)", m => m.Groups[1].Value + m.Groups[1].Value + (m.Groups[2].Length > 0 ? "\\\"" : "")) + "\"";
    }

    void Line(string text) {
        if (IsDisposed) return;
        BeginInvoke((Action)delegate {
            if (text.StartsWith("DONE ")) { doneFolder = text.Substring(5); return; }
            if (text.StartsWith("== ")) status.Text = text.Substring(3);
            else if (text.StartsWith("[") && text.Contains("/6]")) status.Text = text;
            log.AppendText(text + Environment.NewLine);
        });
    }

    void SetStatus(string text) { BeginInvoke((Action)delegate { status.Text = text; }); }

    void Finish(bool ok, string reason) {
        if (IsDisposed) return;
        BeginInvoke((Action)delegate {
            bar.Style = ProgressBarStyle.Blocks;
            bar.Value = ok ? 100 : 0;
            start.Enabled = true;
            if (ok && doneFolder != null) {
                status.Text = "Готово";
                openOut.Visible = true;
            } else {
                status.Text = "Сборка не удалась. Причина в журнале.";
                log.AppendText(reason + Environment.NewLine);
            }
            if (autoLog != null) {
                File.WriteAllText(autoLog, status.Text + Environment.NewLine + log.Text, Encoding.UTF8);
                Close();
            }
        });
    }

    void OnClosing(object sender, FormClosingEventArgs e) {
        Process p = running;
        if (p == null) return;
        if (MessageBox.Show(this, "Сборка ещё идёт. Прервать?", Text, MessageBoxButtons.YesNo) != DialogResult.Yes) { e.Cancel = true; return; }
        try { Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + p.Id) { CreateNoWindow = true, UseShellExecute = false }); } catch (Exception) { }
    }
}
