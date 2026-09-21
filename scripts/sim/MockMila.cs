// Mock MILA robot for testing the controllers without hardware.
// Serves the same endpoints as the ESP8266 firmware (/status, /cmd, /mode,
// /fleet) with simulated sensor data, and logs every command it receives.
//
// USAGE  MockMila.exe [port] [--any]
//
// BUILD + RUN
//   csc /nologo /out:MockMila.exe MockMila.cs && MockMila.exe [port]
//   (csc lives in %WINDIR%\Microsoft.NET\Framework64\v4.0.30319\)
//
// Then point a controller at it:  --host 127.0.0.1 --port 5010
//
// Extra test hook: GET /ir?v=0x74 presses an IR remote key (0x74 fwd, 0x75 back, 0x34 left, 0x33 right).
// Like the real receiver it holds 350 ms and then stops, and it moves the motors without changing `cmd`,
// which is how IR driving looks to the firmware. /status also reports the track state as ml / mr.
// Binds to loopback only (no firewall prompt). Pass --any as a 2nd argument to
// bind every interface, e.g. to test LAN auto-discovery (Windows will ask to allow it).

using System;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

static class MockMila
{
    static readonly object lk = new object();
    static readonly DateTime t0 = DateTime.Now;
    static string mode = "wasd", cmd = "STOP";
    static int speed = 100;
    static string ir = "---";
    static DateTime irUntil = DateTime.MinValue;
    static int irL = 0, irR = 0;

    static void Main(string[] args)
    {
        int port = 5010;
        if (args.Length > 0) int.TryParse(args[0], out port);
        bool any = args.Length > 1 && args[1] == "--any";
        TcpListener l = new TcpListener(any ? IPAddress.Any : IPAddress.Loopback, port);
        l.Start();
        Console.WriteLine("Mock MILA listening on " + (any ? "all interfaces" : "127.0.0.1") + " port " + port);
        while (true)
        {
            TcpClient c = l.AcceptTcpClient();
            ThreadPool.QueueUserWorkItem(delegate { Handle(c); });
        }
    }

    static string F(double v, string fmt) { return v.ToString(fmt, CultureInfo.InvariantCulture); }

    // Track state (-1 back, 0 off, +1 forward) for a web command, matching the firmware's motor functions.
    // (Tank commands drive one track and leave the other alone; the mock keeps only the last command.)
    static void MotorsFor(string c, out int l, out int r)
    {
        l = 0; r = 0;
        switch (c)
        {
            case "FORWARD": l = 1; r = 1; break;
            case "BACKWARD": l = -1; r = -1; break;
            case "LEFT": l = -1; r = 1; break;
            case "RIGHT": l = 1; r = -1; break;
            case "L_FWD": l = 1; break;
            case "L_BWD": l = -1; break;
            case "R_FWD": r = 1; break;
            case "R_BWD": r = -1; break;
        }
    }

    // Dev-only hook: press an IR remote key. Like the real receiver, a WASD key holds for 350 ms and then
    // the robot stops. IR moves the motors directly, so `cmd` (web commands only) does not change.
    static void PressIr(string code)
    {
        lock (lk)
        {
            ir = code;
            int l = 0, r = 0;
            switch (code.ToLowerInvariant())
            {
                case "0x74": l = 1; r = 1; break;     // forward
                case "0x75": l = -1; r = -1; break;   // backward
                case "0x34": l = -1; r = 1; break;    // left
                case "0x33": l = 1; r = -1; break;    // right
            }
            if (l != 0 || r != 0) { irL = l; irR = r; irUntil = DateTime.Now.AddMilliseconds(350); }
        }
    }

    static string Status()
    {
        double t = (DateTime.Now - t0).TotalSeconds;
        double dist = 60 + 55 * Math.Sin(t * 0.5);
        string left = F(70 + 45 * Math.Sin(t * 0.3 + 1), "0");
        string right = F(55 + 50 * Math.Sin(t * 0.21 + 2), "0");
        if (((int)t % 25) >= 23) left = "---";   // firmware reports "---" when a side reading is missing
        double temp = 24 + 1.5 * Math.Sin(t / 20);
        double hum = 45 + 8 * Math.Sin(t / 13);
        lock (lk)
        {
            int ml, mr;
            if (DateTime.Now < irUntil) { ml = irL; mr = irR; }   // an IR key is being held
            else MotorsFor(cmd, out ml, out mr);
            return "{\"mode\":\"" + mode + "\",\"cmd\":\"" + cmd + "\",\"dist\":" + F(dist, "0.00") +
                   ",\"left\":\"" + left + "\",\"right\":\"" + right + "\",\"turn\":\"---\",\"ir\":\"" + ir + "\"" +
                   ",\"temp\":" + F(temp, "0.0") + ",\"hum\":" + F(hum, "0.0") +
                   ",\"ip\":\"mock\",\"fleet\":0,\"speed\":" + speed + ",\"guard\":0" +
                   ",\"ml\":" + ml + ",\"mr\":" + mr + "}";
        }
    }

    static void Handle(TcpClient c)
    {
        using (c)
        {
            try
            {
                c.ReceiveTimeout = 2000;
                NetworkStream s = c.GetStream();
                StreamReader rd = new StreamReader(s, Encoding.ASCII);
                string line = rd.ReadLine();
                if (line == null) return;
                string h;
                while ((h = rd.ReadLine()) != null && h.Length > 0) { }

                string[] parts = line.Split(' ');
                string target = parts.Length > 1 ? parts[1] : "/";
                string path = target, query = "";
                int q = target.IndexOf('?');
                if (q >= 0) { path = target.Substring(0, q); query = target.Substring(q + 1); }
                string v = query.StartsWith("v=") ? Uri.UnescapeDataString(query.Substring(2)) : "";

                string body = "OK", type = "text/plain";
                if (path == "/status") { body = Status(); type = "application/json"; }
                else if (path == "/fleet") { body = "{\"authority\":\"none\",\"robots\":[]}"; type = "application/json"; }
                else if (path == "/cmd")
                {
                    lock (lk)
                    {
                        if (v.StartsWith("SPEED:")) { int.TryParse(v.Substring(6), out speed); speed = Math.Max(0, Math.Min(100, speed)); }
                        else if (v != "SPEEDCYCLE") cmd = v;
                    }
                    Console.WriteLine("{0:HH:mm:ss.fff}  CMD   {1}", DateTime.Now, v);
                }
                else if (path == "/ir")
                {
                    PressIr(v);
                    Console.WriteLine("{0:HH:mm:ss.fff}  IR    {1}", DateTime.Now, v);
                }
                else if (path == "/mode")
                {
                    lock (lk) { mode = v; }
                    Console.WriteLine("{0:HH:mm:ss.fff}  MODE  {1}", DateTime.Now, v);
                }
                else if (path != "/") body = "not found";

                byte[] b = Encoding.UTF8.GetBytes(body);
                string head = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + b.Length +
                              "\r\nConnection: close\r\n\r\n";
                byte[] hb = Encoding.ASCII.GetBytes(head);
                s.Write(hb, 0, hb.Length);
                s.Write(b, 0, b.Length);
            }
            catch (Exception) { }
        }
    }
}
