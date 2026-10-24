using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Windows.Forms;

namespace MMMergeAssets
{
    public sealed class BuilderForm : Form
    {
        readonly TextBox[] folders = {new TextBox(), new TextBox(), new TextBox(), new TextBox()};
        readonly Button start = new Button();
        readonly TextBox log = new TextBox();
        readonly ProgressBar progress = new ProgressBar();
        Process process;
        bool building;
        string selectedOutput;

        public BuilderForm()
        {
            Text = "MMMerge Vita Asset Builder 0.3.0";
            ClientSize = new Size(860, 650);
            MinimumSize = new Size(800, 650);
            AutoScaleMode = AutoScaleMode.Dpi;
            Font = new Font("Segoe UI", 10);
            Controls.Add(new Label {Text = "MMMerge Vita · Asset Builder", AutoSize = true,
                Font = new Font("Segoe UI", 20, FontStyle.Bold), Location = new Point(20, 16)});
            Controls.Add(new Label {Text = "Create all five game-data archives from your installed English GOG games.\n" +
                "No separate MMMerge or OpenYAMM installation is needed. Original files are only read.",
                Location = new Point(22, 65), Size = new Size(815, 48)});
            string[] labels = {"Might and Magic VI", "Might and Magic VII", "Might and Magic VIII", "New / empty output folder"};
            for (int i = 0; i < folders.Length; ++i)
            {
                int index = i;
                int y = 125 + i * 40;
                Controls.Add(new Label {Text = labels[i], Location = new Point(22, y + 5), Size = new Size(200, 26)});
                folders[i].SetBounds(228, y, 510, 29);
                folders[i].Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
                Controls.Add(folders[i]);
                Button browse = new Button {Text = "Browse…", Location = new Point(748, y - 1), Size = new Size(90, 30),
                    Anchor = AnchorStyles.Top | AnchorStyles.Right};
                browse.Click += delegate {
                    if (building) return;
                    using (FolderBrowserDialog picker = new FolderBrowserDialog())
                    {
                        picker.Description = labels[index];
                        if (picker.ShowDialog(this) == DialogResult.OK) folders[index].Text = picker.SelectedPath;
                    }
                };
                Controls.Add(browse);
            }
            Controls.Add(new Label {Text = "Allow at least 8 GiB free on the output drive. Video conversion can take several minutes.\n" +
                "The bundled supplement supplies merged scripts, tables, scenes and additional media; most data comes from your games.",
                Location = new Point(22, 294), Size = new Size(815, 48)});
            start.Text = "Build complete game data";
            start.SetBounds(22, 352, 220, 36);
            start.Click += StartBuild;
            Controls.Add(start);
            Button save = new Button {Text = "Save log…", Location = new Point(252, 352), Size = new Size(110, 36)};
            save.Click += delegate {
                using (SaveFileDialog picker = new SaveFileDialog {Filter = "Text file|*.txt", FileName = "MMMerge-builder-log.txt"})
                    if (picker.ShowDialog(this) == DialogResult.OK) File.WriteAllText(picker.FileName, log.Text, Encoding.UTF8);
            };
            Controls.Add(save);
            progress.SetBounds(375, 358, 463, 25);
            progress.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            progress.Style = ProgressBarStyle.Marquee;
            progress.Visible = false;
            Controls.Add(progress);
            log.SetBounds(22, 403, 816, 225);
            log.Multiline = true;
            log.ReadOnly = true;
            log.ScrollBars = ScrollBars.Both;
            log.WordWrap = false;
            log.Font = new Font("Consolas", 9);
            log.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            Controls.Add(log);
            FormClosing += delegate(object sender, FormClosingEventArgs e) {
                if (!building) return;
                e.Cancel = true;
                MessageBox.Show(this, "Please wait for the current build to finish. The original games are not being changed.",
                    "Conversion in progress", MessageBoxButtons.OK, MessageBoxIcon.Information);
            };
        }

        static string Quote(string value)
        {
            StringBuilder result = new StringBuilder("\"");
            int slashes = 0;
            foreach (char c in value)
            {
                if (c == '\\') { ++slashes; continue; }
                if (c == '"') result.Append('\\', slashes * 2 + 1);
                else result.Append('\\', slashes);
                slashes = 0;
                result.Append(c);
            }
            result.Append('\\', slashes * 2);
            return result.Append('"').ToString();
        }

        void Append(string value)
        {
            if (IsDisposed) return;
            if (InvokeRequired) { BeginInvoke(new Action<string>(Append), value); return; }
            log.AppendText(value + Environment.NewLine);
        }

        void Finish(bool success, string message)
        {
            if (InvokeRequired) { BeginInvoke(new Action<bool, string>(Finish), success, message); return; }
            building = false;
            process = null;
            progress.Visible = false;
            start.Enabled = true;
            foreach (TextBox box in folders) box.ReadOnly = false;
            MessageBox.Show(this, message, success ? "Game data complete" : "Build stopped",
                MessageBoxButtons.OK, success ? MessageBoxIcon.Information : MessageBoxIcon.Error);
        }

        void StartBuild(object sender, EventArgs e)
        {
            foreach (TextBox box in folders)
                if (String.IsNullOrWhiteSpace(box.Text))
                {
                    MessageBox.Show(this, "Select all three installation folders and a new or empty output folder.");
                    return;
                }
            string core = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "MMMergeBuilderCore.exe");
            if (!File.Exists(core)) { MessageBox.Show(this, "Extract the entire tool ZIP first; MMMergeBuilderCore.exe is missing."); return; }
            selectedOutput = folders[3].Text.Trim();
            ProcessStartInfo info = new ProcessStartInfo {
                FileName = core, Arguments = "--mm6 " + Quote(folders[0].Text.Trim()) + " --mm7 " + Quote(folders[1].Text.Trim()) +
                    " --mm8 " + Quote(folders[2].Text.Trim()) + " --output " + Quote(selectedOutput),
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
            };
            building = true;
            start.Enabled = false;
            foreach (TextBox box in folders) box.ReadOnly = true;
            progress.Visible = true;
            Append("Starting complete asset build…");
            Process child = new Process {StartInfo = info};
            process = child;
            child.OutputDataReceived += delegate(object s, DataReceivedEventArgs a) {if (a.Data != null) Append(a.Data);};
            child.ErrorDataReceived += delegate(object s, DataReceivedEventArgs a) {if (a.Data != null) Append(a.Data);};
            try
            {
                child.Start();
                child.BeginOutputReadLine();
                child.BeginErrorReadLine();
                System.Threading.ThreadPool.QueueUserWorkItem(delegate {
                    child.WaitForExit(); // Also drains redirected line events before reporting completion.
                    int code = child.ExitCode;
                    child.Dispose();
                    Finish(code == 0, code == 0 ? "All five archives were verified and created in:\n" + selectedOutput +
                        "\n\nRead INSTALL-DATA.txt in that folder." : "The conversion stopped. See the log for the exact file or input problem.\n" +
                        "No partial game-data set was published; your original games were not modified.");
                });
            }
            catch (Exception error) {child.Dispose(); Finish(false, error.Message);}
        }
    }

    internal static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new BuilderForm());
        }
    }
}
