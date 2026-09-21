// MILA WiFi controller (WinForms) — C# port of mila_controller.py.
// Mirrors the onboard web dashboard. No NuGet dependencies.
//
// USAGE
//   MilaController.exe
//   MilaController.exe --host 192.168.4.1 --port 5010
//
//   If MILA joined NORA's network (fleet mode) instead of hosting her own
//   AP, she won't be at 192.168.4.1 anymore — check NORA's dashboard/fleet
//   registry (http://192.168.4.1:5000/robots) for MILA's actual IP and pass
//   it with --host.
//
// BUILD
//   dotnet build -c Release            (see MilaController.csproj)
//   dotnet publish -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
//
// CONTROLS
//   1 / 2 / 3     WASD / TANK / OBSTACLE mode
//   WASD mode:    Arrow keys or WASD to drive, Space to stop
//   TANK mode:    Q/A = left track fwd/back, E/D = right track fwd/back
//   Esc           quit
//   Everything is also clickable with the mouse.

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Globalization;
using System.Net.Http;
using System.Threading;
using System.Windows.Forms;

namespace Mila
{
    // ------------------------------------------------------------------------
    // Transport
    // ------------------------------------------------------------------------

    class WifiLink : IDisposable
    {
        readonly string baseUrl;
        readonly HttpClient http;
        readonly object lk = new object();
        readonly BlockingCollection<string> queue = new BlockingCollection<string>();
        Dictionary<string, string> data = new Dictionary<string, string>();
        volatile bool ok;
        volatile bool running = true;

        public WifiLink(string host, int port)
        {
            baseUrl = "http://" + host + ":" + port;
            http = new HttpClient();
            http.Timeout = TimeSpan.FromSeconds(1);

            Thread poller = new Thread(Poll);
            poller.IsBackground = true;
            poller.Start();

            // One worker keeps commands in order (a STOP must never overtake
            // the FORWARD that preceded it).
            Thread worker = new Thread(Work);
            worker.IsBackground = true;
            worker.Start();
        }

        public bool Ok { get { return ok; } }

        public Dictionary<string, string> Telemetry()
        {
            lock (lk) { return new Dictionary<string, string>(data); }
        }

        public void Cmd(string action) { queue.Add("/cmd?v=" + action); }
        public void Mode(string m) { queue.Add("/mode?v=" + m); }

        // Synchronous STOP for shutdown, when the worker may not get to run.
        public void StopNow()
        {
            try { http.GetStringAsync(baseUrl + "/cmd?v=STOP").Wait(); } catch (Exception) { }
        }

        void Work()
        {
            foreach (string path in queue.GetConsumingEnumerable())
            {
                try { http.GetStringAsync(baseUrl + path).Wait(); }
                catch (Exception)
                {
                    // Offline: drop the backlog rather than replaying stale drive
                    // commands when the link comes back.
                    string dropped;
                    while (queue.TryTake(out dropped)) { }
                }
            }
        }

        void Poll()
        {
            while (running)
            {
                try
                {
                    string body = http.GetStringAsync(baseUrl + "/status").Result;
                    Dictionary<string, string> parsed = ParseFlatJson(body);
                    lock (lk) { data = parsed; }
                    ok = true;
                }
                catch (Exception) { ok = false; }
                Thread.Sleep(400);
            }
        }

        // Minimal parser for the firmware's flat /status JSON.
        static Dictionary<string, string> ParseFlatJson(string s)
        {
            Dictionary<string, string> o = new Dictionary<string, string>();
            int i = 0, n = s.Length;
            while (i < n)
            {
                while (i < n && s[i] != '"') i++;
                if (i >= n) break;
                string key = ReadString(s, ref i);
                while (i < n && s[i] != ':') i++;
                i++;
                while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
                string val;
                if (i < n && s[i] == '"') val = ReadString(s, ref i);
                else
                {
                    int start = i;
                    while (i < n && s[i] != ',' && s[i] != '}') i++;
                    val = s.Substring(start, i - start).Trim();
                }
                o[key] = val;
            }
            return o;
        }

        static string ReadString(string s, ref int i)
        {
            System.Text.StringBuilder sb = new System.Text.StringBuilder();
            i++;
            while (i < s.Length && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < s.Length) i++;
                sb.Append(s[i++]);
            }
            i++;
            return sb.ToString();
        }

        public void Dispose()
        {
            running = false;
            queue.CompleteAdding();
            http.Dispose();
        }
    }

    // ------------------------------------------------------------------------
    // UI — same dark/cyan palette as the onboard dashboard
    // ------------------------------------------------------------------------

    class Btn
    {
        public Rectangle Rect;
        public string Label;
        public int Group;   // 0 always, 1 WASD only, 2 TANK only
        public Action Cb;
        public bool Active, Pressed;
    }

    class MainForm : Form
    {
        static readonly Color BG = Color.FromArgb(10, 10, 15);
        static readonly Color PANEL = Color.FromArgb(18, 18, 26);
        static readonly Color BORDER = Color.FromArgb(30, 30, 46);
        static readonly Color ACCENT = Color.FromArgb(0, 212, 255);
        static readonly Color RED = Color.FromArgb(255, 68, 68);
        static readonly Color GREEN = Color.FromArgb(0, 255, 136);
        static readonly Color TEXT = Color.FromArgb(224, 224, 240);
        static readonly Color DIM = Color.FromArgb(102, 102, 136);
        static readonly Color ACCENT_BG = Color.FromArgb(14, 34, 46);

        const int W = 420, H = 720;

        readonly WifiLink link;
        readonly string host;
        readonly int port;
        readonly List<Btn> btns = new List<Btn>();
        readonly Btn[] modeBtns = new Btn[3];
        readonly HashSet<Keys> down = new HashSet<Keys>();
        readonly Font fBig, fMed, fSml;
        string mode = "wasd";           // mirrors the firmware's WASD boot default
        string activeDrive = null;
        int speedPct = 100;

        public MainForm(string host, int port)
        {
            this.host = host;
            this.port = port;
            link = new WifiLink(host, port);

            Text = "MILA Control";
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            ClientSize = new Size(W, H);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = BG;
            KeyPreview = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer, true);

            fBig = new Font("Consolas", 26f, FontStyle.Bold, GraphicsUnit.Pixel);
            fMed = new Font("Consolas", 15f, FontStyle.Bold, GraphicsUnit.Pixel);
            fSml = new Font("Consolas", 12f, FontStyle.Regular, GraphicsUnit.Pixel);

            BuildButtons();

            System.Windows.Forms.Timer t = new System.Windows.Forms.Timer();
            t.Interval = 16;
            t.Tick += delegate { Invalidate(); };
            t.Start();
        }

        // ---- actions ----

        void StartDrive(string action) { activeDrive = action; link.Cmd(action); }

        void StopDrive()
        {
            if (activeDrive != null) { activeDrive = null; link.Cmd("STOP"); }
        }

        void SetMode(string m)
        {
            mode = m;
            link.Mode(m);
            string[] names = { "wasd", "tank", "obstacle" };
            for (int i = 0; i < 3; i++) modeBtns[i].Active = (names[i] == m);
        }

        // Speed presets mirror the firmware's speedPresets[] (100/75/50/25).
        void AdjustSpeed(int delta)
        {
            speedPct = Math.Max(25, Math.Min(100, speedPct + delta));
            link.Cmd("SPEED:" + speedPct);
        }

        Btn Add(int x, int y, int w, int h, string label, Action cb, int group, bool active)
        {
            Btn b = new Btn();
            b.Rect = new Rectangle(x, y, w, h);
            b.Label = label;
            b.Cb = cb;
            b.Group = group;
            b.Active = active;
            btns.Add(b);
            return b;
        }

        void BuildButtons()
        {
            modeBtns[0] = Add(20, 100, 120, 40, "WASD", delegate { SetMode("wasd"); }, 0, true);
            modeBtns[1] = Add(150, 100, 120, 40, "TANK", delegate { SetMode("tank"); }, 0, false);
            modeBtns[2] = Add(280, 100, 120, 40, "OBSTACLE", delegate { SetMode("obstacle"); }, 0, false);

            Add(20, 150, 180, 40, "SPD -", delegate { AdjustSpeed(-25); }, 0, false);
            Add(220, 150, 180, 40, "SPD +", delegate { AdjustSpeed(25); }, 0, false);

            // WASD d-pad
            int cx = W / 2, cy = 280, bs = 70, gap = 8;
            Add(cx - bs / 2, cy - bs - gap, bs, bs, "^", delegate { StartDrive("FORWARD"); }, 1, false);
            Add(cx - bs / 2, cy + gap, bs, bs, "v", delegate { StartDrive("BACKWARD"); }, 1, false);
            Add(cx - bs - gap - bs / 2 - gap, cy - bs / 2, bs, bs, "<", delegate { StartDrive("LEFT"); }, 1, false);
            Add(cx + gap + bs / 2 + gap, cy - bs / 2, bs, bs, ">", delegate { StartDrive("RIGHT"); }, 1, false);
            Add(cx - bs / 2, cy - bs / 2, bs, bs, "STOP", delegate { link.Cmd("STOP"); }, 1, false);

            // TANK track buttons
            int ty = 280;
            Add(60, ty - 40, 90, 50, "Q\nL-FWD", delegate { StartDrive("L_FWD"); }, 2, false);
            Add(60, ty + 40, 90, 50, "A\nL-BWD", delegate { StartDrive("L_BWD"); }, 2, false);
            Add(W - 150, ty - 40, 90, 50, "E\nR-FWD", delegate { StartDrive("R_FWD"); }, 2, false);
            Add(W - 150, ty + 40, 90, 50, "D\nR-BWD", delegate { StartDrive("R_BWD"); }, 2, false);
        }

        bool IsShown(Btn b)
        {
            return b.Group == 0 || (b.Group == 1 && mode == "wasd") || (b.Group == 2 && mode == "tank");
        }

        string KeyDrive(Keys k)
        {
            if (mode == "wasd")
            {
                switch (k)
                {
                    case Keys.Up: case Keys.W: return "FORWARD";
                    case Keys.Down: case Keys.S: return "BACKWARD";
                    case Keys.Left: case Keys.A: return "LEFT";
                    case Keys.Right: case Keys.D: return "RIGHT";
                }
            }
            else if (mode == "tank")
            {
                switch (k)
                {
                    case Keys.Q: return "L_FWD";
                    case Keys.A: return "L_BWD";
                    case Keys.E: return "R_FWD";
                    case Keys.D: return "R_BWD";
                }
            }
            return null;
        }

        // ---- input ----

        protected override bool IsInputKey(Keys keyData)
        {
            return true;    // deliver arrow keys to OnKeyDown
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            e.Handled = true;
            if (!down.Add(e.KeyCode)) return;   // ignore OS auto-repeat

            if (e.KeyCode == Keys.Escape) Close();
            else if (e.KeyCode == Keys.D1) SetMode("wasd");
            else if (e.KeyCode == Keys.D2) SetMode("tank");
            else if (e.KeyCode == Keys.D3) SetMode("obstacle");
            else if (e.KeyCode == Keys.Space) StopDrive();
            else
            {
                string a = KeyDrive(e.KeyCode);
                if (a != null) StartDrive(a);
            }
        }

        protected override void OnKeyUp(KeyEventArgs e)
        {
            e.Handled = true;
            down.Remove(e.KeyCode);
            string a = KeyDrive(e.KeyCode);
            if (a != null && a == activeDrive) StopDrive();
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button != MouseButtons.Left) return;
            foreach (Btn b in btns)
            {
                if (IsShown(b) && b.Rect.Contains(e.Location))
                {
                    b.Pressed = true;
                    b.Cb();
                    break;
                }
            }
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            ReleaseAll();
        }

        // Don't leave the robot driving if the window loses focus mid-press.
        protected override void OnDeactivate(EventArgs e)
        {
            base.OnDeactivate(e);
            down.Clear();
            ReleaseAll();
        }

        void ReleaseAll()
        {
            foreach (Btn b in btns) b.Pressed = false;
            StopDrive();
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            base.OnFormClosed(e);
            link.StopNow();
            link.Dispose();
        }

        // ---- drawing ----

        static GraphicsPath RoundRect(Rectangle r, int radius)
        {
            int d = radius * 2;
            GraphicsPath p = new GraphicsPath();
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        static void Panel(Graphics g, Rectangle r, Color fill, Color line, float lineW, int radius)
        {
            using (GraphicsPath p = RoundRect(r, radius))
            using (SolidBrush br = new SolidBrush(fill))
            using (Pen pen = new Pen(line, lineW))
            {
                g.FillPath(br, p);
                g.DrawPath(pen, p);
            }
        }

        static void Str(Graphics g, string s, Font f, Color c, float x, float y, StringAlignment h, StringAlignment v)
        {
            using (SolidBrush br = new SolidBrush(c))
            using (StringFormat sf = new StringFormat(StringFormat.GenericTypographic))
            {
                sf.Alignment = h;
                sf.LineAlignment = v;
                g.DrawString(s, f, br, x, y, sf);
            }
        }

        static string Get(Dictionary<string, string> t, string k)
        {
            string v;
            return t.TryGetValue(k, out v) ? v : "--";
        }

        void Tile(Graphics g, int x, int y, int w, string label, string value)
        {
            Panel(g, new Rectangle(x, y, w, 46), PANEL, BORDER, 1f, 6);
            Str(g, label, fSml, DIM, x + 8, y + 6, StringAlignment.Near, StringAlignment.Near);
            Str(g, value, fMed, ACCENT, x + 8, y + 22, StringAlignment.Near, StringAlignment.Near);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            g.Clear(BG);

            Dictionary<string, string> t = link.Telemetry();
            string sp;
            double spv;
            if (t.TryGetValue("speed", out sp) &&
                double.TryParse(sp, NumberStyles.Float, CultureInfo.InvariantCulture, out spv))
                speedPct = (int)spv;

            Str(g, "MILA", fBig, ACCENT, W / 2f, 28, StringAlignment.Center, StringAlignment.Center);
            bool ok = link.Ok;
            Str(g, host + ":" + port + "   " + (ok ? "connected" : "OFFLINE"), fSml, ok ? GREEN : RED,
                W / 2f, 54, StringAlignment.Center, StringAlignment.Center);

            foreach (Btn b in btns)
            {
                if (!IsShown(b)) continue;
                bool on = b.Active || b.Pressed;
                Panel(g, b.Rect, on ? ACCENT_BG : PANEL, on ? ACCENT : BORDER, 2f, 8);
                Str(g, b.Label, fMed, on ? ACCENT : TEXT,
                    b.Rect.X + b.Rect.Width / 2f, b.Rect.Y + b.Rect.Height / 2f,
                    StringAlignment.Center, StringAlignment.Center);
            }

            int sy = 400, colw = (W - 50) / 2, c2 = 30 + colw;
            Tile(g, 20, sy, colw, "FRONT CM", Get(t, "dist"));
            Tile(g, c2, sy, colw, "LEFT CM", Get(t, "left"));
            Tile(g, 20, sy + 54, colw, "RIGHT CM", Get(t, "right"));
            Tile(g, c2, sy + 54, colw, "LAST CMD", Get(t, "cmd"));
            Tile(g, 20, sy + 108, colw, "LAST IR", Get(t, "ir"));
            Tile(g, c2, sy + 108, colw, "TEMP C", Get(t, "temp"));
            Tile(g, 20, sy + 162, colw, "HUMIDITY %", Get(t, "hum"));
            Tile(g, c2, sy + 162, colw, "IP  " + Get(t, "ip"),
                 Get(t, "fleet") == "1" ? "FLEET (joined NORA)" : "STANDALONE AP");
            Tile(g, 20, sy + 216, colw, "SPEED %", Get(t, "speed"));
            Tile(g, c2, sy + 216, colw, "GUARD", Get(t, "guard") == "1" ? "ACTIVE" : "clear");

            Str(g, "1/2/3 mode  -  arrows/WASD or QAED drive  -  space stop  -  esc quit", fSml, DIM,
                W / 2f, H - 18, StringAlignment.Center, StringAlignment.Center);
        }
    }

    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            string host = "192.168.4.1";
            int port = 5010;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--host" && i + 1 < args.Length) host = args[++i];
                else if (args[i] == "--port" && i + 1 < args.Length) int.TryParse(args[++i], out port);
            }
            Application.EnableVisualStyles();
            Application.Run(new MainForm(host, port));
        }
    }
}
