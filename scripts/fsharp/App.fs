namespace Mila

open System
open System.Collections.Generic
open System.Drawing
open System.Drawing.Drawing2D
open System.Drawing.Text
open System.Globalization
open System.Windows.Forms

/// Same dark/cyan palette as the onboard dashboard.
module Theme =
    let bg = Color.FromArgb(10, 10, 15)
    let panel = Color.FromArgb(18, 18, 26)
    let border = Color.FromArgb(30, 30, 46)
    let accent = Color.FromArgb(0, 212, 255)
    let red = Color.FromArgb(255, 68, 68)
    let amber = Color.FromArgb(255, 190, 60)
    let green = Color.FromArgb(0, 255, 136)
    let text = Color.FromArgb(224, 224, 240)
    let dim = Color.FromArgb(102, 102, 136)
    let accentBg = Color.FromArgb(14, 34, 46)

type Fonts = { Big: Font; Med: Font; Sml: Font }

module Draw =

    let private roundRect (r: Rectangle) (radius: int) =
        let d = float32 (radius * 2)
        let p = new GraphicsPath()
        p.AddArc(float32 r.X, float32 r.Y, d, d, 180.0f, 90.0f)
        p.AddArc(float32 r.Right - d, float32 r.Y, d, d, 270.0f, 90.0f)
        p.AddArc(float32 r.Right - d, float32 r.Bottom - d, d, d, 0.0f, 90.0f)
        p.AddArc(float32 r.X, float32 r.Bottom - d, d, d, 90.0f, 90.0f)
        p.CloseFigure()
        p

    let panel (g: Graphics) (r: Rectangle) (fill: Color) (line: Color) (lineW: float32) (radius: int) =
        use p = roundRect r radius
        use br = new SolidBrush(fill)
        use pen = new Pen(line, lineW)
        g.FillPath(br, p)
        g.DrawPath(pen, p)

    let str (g: Graphics) (s: string) (f: Font) (c: Color) (x: int) (y: int) (h: StringAlignment) (v: StringAlignment) =
        use br = new SolidBrush(c)
        use sf = new StringFormat(StringFormat.GenericTypographic)
        sf.Alignment <- h
        sf.LineAlignment <- v
        g.DrawString(s, f, br, float32 x, float32 y, sf)

    let private near = StringAlignment.Near
    let private centre = StringAlignment.Center
    let private far = StringAlignment.Far

    let tile (g: Graphics) (f: Fonts) (x: int) (y: int) (w: int) (label: string) (value: string) =
        panel g (Rectangle(x, y, w, 46)) Theme.panel Theme.border 1f 6
        str g label f.Sml Theme.dim (x + 8) (y + 6) near near
        str g value f.Med Theme.accent (x + 8) (y + 22) near near

    let private mix (a: Color) (b: Color) (t: float) =
        let ch (x: byte) (y: byte) = int (Math.Round(float x * t + float y * (1.0 - t)))
        Color.FromArgb(ch a.R b.R, ch a.G b.G, ch a.B b.B)

    let private proxColor (cm: float) =
        if cm < 20.0 then Theme.red
        elif cm < 50.0 then Theme.amber
        else Theme.green

    let fmt (v: float) (format: string) =
        if Double.IsNaN v then "--" else v.ToString(format, CultureInfo.InvariantCulture)

    /// Radar: three wedges (left / front / right) whose length is the distance, coloured by proximity.
    let radar (g: Graphics) (f: Fonts) (area: Rectangle) (left: float) (front: float) (right: float) (live: bool) =
        panel g area Theme.panel Theme.border 1f 8
        str g "PROXIMITY" f.Sml Theme.dim (area.Left + 12) (area.Top + 8) near near

        let maxRange = 100.0
        let cx = (area.Left + area.Right) / 2
        let cy = area.Bottom - 36
        let radius = 170

        use ring = new Pen(Theme.border, 1f)
        for i in 1..4 do
            let r = radius * i / 4
            g.DrawArc(ring, cx - r, cy - r, 2 * r, 2 * r, 180, 180) // top half
        g.DrawLine(ring, cx - radius, cy, cx + radius, cy)

        // GDI+ angles run clockwise from +x, ours run counter-clockwise: start = -a2, sweep = a2 - a1.
        let wedges =
            [ ("LEFT", left, 125, 165, area.Left + 80)
              ("FRONT", front, 70, 110, cx)
              ("RIGHT", right, 15, 55, area.Right - 80) ]

        for (name, v, a1, a2, labelX) in wedges do
            let known = not (Double.IsNaN v)
            let col = if not known || not live then Theme.dim else proxColor v
            if known then
                let r = int (Math.Round(float radius * max 0.06 (min 1.0 (v / maxRange))))
                use br = new SolidBrush(mix col Theme.panel 0.35)
                use pen = new Pen(col, 2f)
                g.FillPie(br, cx - r, cy - r, 2 * r, 2 * r, -a2, a2 - a1)
                g.DrawPie(pen, cx - r, cy - r, 2 * r, 2 * r, -a2, a2 - a1)
            str g name f.Sml Theme.dim labelX (cy + 6) centre near
            str g (if known then fmt v "0" + " cm" else "--") f.Med col labelX (cy + 18) centre near

        // the robot
        use tb = new SolidBrush(Theme.accent)
        g.FillPolygon(tb, [| Point(cx, cy - 10); Point(cx - 7, cy + 2); Point(cx + 7, cy + 2) |])

    /// Line chart over the last 150 samples. Each series is scaled to lo..hi (lo = hi means auto).
    let graph
        (g: Graphics)
        (f: Fonts)
        (box: Rectangle)
        (title: string)
        (series: (Color * float list) list)
        (lo: float)
        (hi: float)
        (latest: string list)
        =
        panel g box Theme.panel Theme.border 1f 8
        str g title f.Sml Theme.dim (box.Left + 12) (box.Top + 7) near near

        // latest values, right-aligned, last series first
        let mutable rx = box.Right - 12
        for ((col, _), text) in List.zip series latest |> List.rev do
            let w = g.MeasureString(text, f.Med, 1000, StringFormat.GenericTypographic).Width
            str g text f.Med col rx (box.Top + 5) far near
            rx <- rx - int w - 14

        let plot = Rectangle(box.Left + 12, box.Top + 28, box.Width - 24, box.Height - 38)
        use grid = new Pen(Theme.border, 1f)
        for i in 0..2 do
            let y = plot.Top + plot.Height * i / 2
            g.DrawLine(grid, plot.Left, y, plot.Right, y)

        for (col, values) in series do
            let vals = Array.ofList values
            let range =
                if lo <> hi then
                    Some(lo, hi)
                else
                    // auto-scale this series with a little headroom
                    let valid = vals |> Array.filter (fun v -> not (Double.IsNaN v))
                    if valid.Length = 0 then
                        None
                    else
                        let mn = Array.min valid
                        let mx = Array.max valid
                        if mx - mn < 4.0 then
                            let mid = (mx + mn) / 2.0
                            Some(mid - 2.0, mid + 2.0)
                        else
                            Some(mn, mx)

            match range with
            | None -> ()
            | Some(mn, mx) ->
                use pen = new Pen(col, 2f)
                let run = List<PointF>()

                let flush () =
                    if run.Count > 1 then g.DrawLines(pen, run.ToArray())
                    run.Clear()

                for i in 0 .. vals.Length do
                    if i = vals.Length || Double.IsNaN(vals.[i]) then
                        flush ()
                    else
                        let x = float32 plot.Left + float32 plot.Width * float32 i / 149.0f
                        let frac = max 0.0 (min 1.0 ((vals.[i] - mn) / (mx - mn)))
                        run.Add(PointF(x, float32 plot.Bottom - float32 (float plot.Height * frac)))

/// A clickable button. Visibility, highlight and label are functions so they always reflect current state.
[<ReferenceEquality>]
type Btn =
    { Rect: Rectangle
      Label: unit -> string
      Show: unit -> bool
      On: unit -> bool
      Act: unit -> unit
      mutable Pressed: bool }

type MainForm(settings0: Settings, autoHost: bool) as this =
    inherit Form()

    // Layout: left 420px = controls, right 420px = radar / graphs / connection.
    let W = 840
    let H = 720
    let SL_L = 80
    let SL_R = 340
    let SL_Y = 172 // speed slider

    let mutable host = settings0.Host
    let mutable port = settings0.Port
    let mutable mode = settings0.Mode // mirrors the firmware's WASD boot default
    let mutable modeSynced = false
    let mutable activeDrive = ""
    let mutable exportMsg = ""

    let link = new WifiLink(settings0.Host, settings0.Port)
    let scanner = Scanner()
    let down = HashSet<Keys>()
    let timer = new System.Windows.Forms.Timer(Interval = 16)

    let fonts =
        { Big = new Font("Consolas", 26.0f, FontStyle.Bold, GraphicsUnit.Pixel)
          Med = new Font("Consolas", 15.0f, FontStyle.Bold, GraphicsUnit.Pixel)
          Sml = new Font("Consolas", 12.0f, FontStyle.Regular, GraphicsUnit.Pixel) }

    // speed
    let mutable speedPct = 100
    let mutable sliderDrag = false
    let mutable speedPending = false
    let mutable speedHold = 0
    let mutable lastSpeedSend = 0

    // gamepad
    let mutable padIndex = -1
    let mutable padNextProbe = 0
    let mutable padLastTick = 0
    let mutable padPrev = 0us
    let mutable padDrive = ""
    let mutable padL = 0
    let mutable padR = 0
    let mutable padSpeedF = 100.0

    // ---- actions ----

    let saveSettings () =
        Settings.save
            { Host = host
              Port = port
              Mode = mode
              HasMode = true }

    let startDrive (action: string) =
        activeDrive <- action
        link.Cmd action

    let stopDrive () =
        if activeDrive <> "" then
            activeDrive <- ""
            link.Cmd "STOP"

    let setMode (m: string) =
        mode <- m
        link.Mode m
        saveSettings ()

    let cycleMode (dir: int) =
        let names = [| "wasd"; "tank"; "obstacle" |]
        let cur = max 0 (Array.IndexOf(names, mode))
        setMode names.[(cur + dir + 3) % 3]

    /// Local speed change: shown immediately, sent (throttled) by flushSpeed.
    let setSpeedLocal (v: int) =
        let v = max 25 (min 100 v)
        if v <> speedPct || speedPending then
            speedPct <- v
            speedPending <- true
            speedHold <- Environment.TickCount + 1500 // don't let stale telemetry yank the slider back

    let flushSpeed () =
        let now = Environment.TickCount
        if speedPending && now - lastSpeedSend >= 80 then
            speedPending <- false
            lastSpeedSend <- now
            link.Cmd("SPEED:" + string speedPct)

    let setSliderFromX (x: int) =
        setSpeedLocal (25 + int (Math.Round(float (x - SL_L) * 75.0 / float (SL_R - SL_L))))

    let connectTo (newHost: string) =
        host <- newHost
        link.Retarget(host, port)
        modeSynced <- false
        saveSettings ()

    let exportCsv () = exportMsg <- Csv.export (link.All())

    // ---- gamepad ----

    let pollPad () =
        let now = Environment.TickCount
        let mutable st = XInputState()

        if padIndex < 0 && now - padNextProbe >= 0 then
            padNextProbe <- now + 1000 // probing empty slots is slow, so only once a second
            let mutable i = 0
            while padIndex < 0 && i < 4 do
                if Native.getState (i, &st) then
                    padIndex <- i
                    padLastTick <- now
                i <- i + 1

        if padIndex >= 0 then
            if not (Native.getState (padIndex, &st)) then
                // Unplugged mid-drive: stop the robot.
                padIndex <- -1
                padDrive <- ""
                padL <- 0
                padR <- 0
                stopDrive ()
            else
                let p = st.Gamepad
                let lx = PadMap.axis p.ThumbLX
                let ly = PadMap.axis p.ThumbLY
                let ry = PadMap.axis p.ThumbRY
                let dt = float (now - padLastTick) / 1000.0
                padLastTick <- now

                // A = stop, LB/RB = previous/next mode (edge-triggered).
                let pressed = p.Buttons &&& ~~~padPrev
                padPrev <- p.Buttons
                if (pressed &&& Buttons.a) <> 0us then
                    padDrive <- ""
                    padL <- 0
                    padR <- 0
                    activeDrive <- ""
                    link.Cmd "STOP"
                if (pressed &&& Buttons.leftBumper) <> 0us then cycleMode (-1)
                if (pressed &&& Buttons.rightBumper) <> 0us then cycleMode (1)

                if mode = "wasd" then
                    let want = PadMap.wasdDirection lx ly p.Buttons
                    if want <> padDrive then
                        padDrive <- want
                        if want = "" then stopDrive () else startDrive want
                elif mode = "tank" then
                    // Left stick Y = left track, right stick Y = right track.
                    let l = PadMap.track ly
                    let r = PadMap.track ry
                    if l <> padL || r <> padR then
                        let cmds, driving = PadMap.tankCommands (padL, padR) (l, r)
                        for c in cmds do
                            link.Cmd c
                        activeDrive <- (if driving then "PAD_TANK" else "") // sentinel so Space / focus loss still sends STOP
                        padL <- l
                        padR <- r
                else
                    padDrive <- ""
                    padL <- 0
                    padR <- 0

                // Triggers ramp speed: RT up, LT down (full pull = 60 %/s).
                let rt = float p.RightTrigger / 255.0
                let lt = float p.LeftTrigger / 255.0
                if rt > 0.1 || lt > 0.1 then
                    padSpeedF <- max 25.0 (min 100.0 (padSpeedF + (rt - lt) * 60.0 * dt))
                    setSpeedLocal (int (Math.Round padSpeedF))
                else
                    padSpeedF <- float speedPct

    // ---- layout ----

    let never () = false
    let always () = true
    let literal (s: string) = fun () -> s

    let btn x y w h label show on act =
        { Rect = Rectangle(x, y, w, h)
          Label = label
          Show = show
          On = on
          Act = act
          Pressed = false }

    let inWasd () = mode = "wasd"
    let inTank () = mode = "tank"

    let modeButton x label name =
        btn x 100 120 40 (literal label) always (fun () -> mode = name) (fun () -> setMode name)

    let buttons : Btn list =
        let cx, cy, bs, gap = 210, 280, 70, 8
        let drive name = fun () -> startDrive name
        let tank x y label name = btn x y 90 50 (literal label) inTank never (drive name)
        let pad x y label name = btn x y bs bs (literal label) inWasd never (drive name)

        [ modeButton 20 "WASD" "wasd"
          modeButton 150 "TANK" "tank"
          modeButton 280 "OBSTACLE" "obstacle"

          // Speed: [-]  slider  [+]. Presets mirror the firmware's speedPresets[] (100/75/50/25).
          btn 20 150 44 40 (literal "-") always never (fun () -> setSpeedLocal (speedPct - 25))
          btn 356 150 44 40 (literal "+") always never (fun () -> setSpeedLocal (speedPct + 25))

          // WASD d-pad
          pad (cx - bs / 2) (cy - bs - gap) "^" "FORWARD"
          pad (cx - bs / 2) (cy + gap) "v" "BACKWARD"
          pad (cx - bs - gap - bs / 2 - gap) (cy - bs / 2) "<" "LEFT"
          pad (cx + gap + bs / 2 + gap) (cy - bs / 2) ">" "RIGHT"
          btn (cx - bs / 2) (cy - bs / 2) bs bs (literal "STOP") inWasd never (fun () -> link.Cmd "STOP")

          // TANK tracks
          tank 60 240 "Q\nL-FWD" "L_FWD"
          tank 60 320 "A\nL-BWD" "L_BWD"
          tank 270 240 "E\nR-FWD" "R_FWD"
          tank 270 320 "D\nR-BWD" "R_BWD"

          // Right column: CSV export, robot discovery.
          btn 440 545 180 36 (literal "EXPORT CSV") always never exportCsv
          btn 440 622 150 34
              (fun () ->
                  if scanner.Scanning then sprintf "SCANNING %d%%" scanner.Percent else "SCAN NETWORK")
              always
              never
              (fun () -> scanner.Start(port, false)) ]
        @ [ for i in 0..2 do
                // up to three discovered robots, shown as click-to-connect buttons
                let ip () = List.tryItem i (scanner.Found())
                yield
                    btn (440 + i * 130) 668 122 34
                        (fun () -> defaultArg (ip ()) "")
                        (fun () -> (ip ()).IsSome)
                        never
                        (fun () -> ip () |> Option.iter connectTo) ]

    let keyDrive (k: Keys) : string =
        match mode, k with
        | "wasd", (Keys.Up | Keys.W) -> "FORWARD"
        | "wasd", (Keys.Down | Keys.S) -> "BACKWARD"
        | "wasd", (Keys.Left | Keys.A) -> "LEFT"
        | "wasd", (Keys.Right | Keys.D) -> "RIGHT"
        | "tank", Keys.Q -> "L_FWD"
        | "tank", Keys.A -> "L_BWD"
        | "tank", Keys.E -> "R_FWD"
        | "tank", Keys.D -> "R_BWD"
        | _ -> ""

    let releaseAll () =
        for b in buttons do
            b.Pressed <- false
        stopDrive ()

    // ---- per-frame work ----

    let onTick () =
        pollPad ()
        flushSpeed ()

        // A robot found by `--host auto`: connect on the UI thread.
        match scanner.TakeAuto() with
        | null -> ()
        | ip -> connectTo ip

        let t = link.Telemetry()

        // Keep the slider in sync with telemetry so it stays correct even if the IR remote or dashboard
        // changed the speed out from under us.
        match Map.tryFind "speed" t with
        | Some sp when not sliderDrag && Environment.TickCount - speedHold > 0 && not speedPending ->
            match Double.TryParse(sp, NumberStyles.Float, CultureInfo.InvariantCulture) with
            | true, v -> speedPct <- int v
            | _ -> ()
        | _ -> ()

        // Once connected: push a remembered mode to the robot, or adopt the robot's current one.
        if not modeSynced && link.Ok then
            let robotMode = Map.tryFind "mode" t
            match robotMode with
            | Some rm when settings0.HasMode -> if rm <> mode then link.Mode mode
            | None when settings0.HasMode -> link.Mode mode
            | Some rm when rm = "wasd" || rm = "tank" || rm = "obstacle" -> mode <- rm
            | _ -> ()
            modeSynced <- true

        this.Invalidate()

    do
        this.Text <- "MILA Control"
        this.FormBorderStyle <- FormBorderStyle.FixedSingle
        this.MaximizeBox <- false
        this.ClientSize <- Size(W, H)
        this.StartPosition <- FormStartPosition.CenterScreen
        this.BackColor <- Theme.bg
        this.KeyPreview <- true
        this.SetStyle(
            ControlStyles.AllPaintingInWmPaint
            ||| ControlStyles.UserPaint
            ||| ControlStyles.OptimizedDoubleBuffer,
            true
        )
        timer.Tick.Add(fun _ -> onTick ())
        timer.Start()
        if autoHost then scanner.Start(port, true)

    // ---- input ----

    override _.IsInputKey(_keyData: Keys) = true // deliver arrow keys to OnKeyDown

    override _.OnKeyDown(e: KeyEventArgs) =
        e.Handled <- true
        if down.Add e.KeyCode then // ignore OS auto-repeat
            match e.KeyCode with
            | Keys.Escape -> this.Close()
            | Keys.D1 -> setMode "wasd"
            | Keys.D2 -> setMode "tank"
            | Keys.D3 -> setMode "obstacle"
            | Keys.Space -> stopDrive ()
            | k ->
                match keyDrive k with
                | "" -> ()
                | a -> startDrive a

    override _.OnKeyUp(e: KeyEventArgs) =
        e.Handled <- true
        down.Remove e.KeyCode |> ignore
        let a = keyDrive e.KeyCode
        if a <> "" && a = activeDrive then stopDrive ()

    override _.OnMouseDown(e: MouseEventArgs) =
        if e.Button = MouseButtons.Left then
            let slider = Rectangle(SL_L - 14, SL_Y - 16, SL_R - SL_L + 28, 32)
            if slider.Contains(e.Location) then
                sliderDrag <- true
                setSliderFromX e.X
            else
                match buttons |> List.tryFind (fun b -> b.Show() && b.Rect.Contains(e.Location)) with
                | Some b ->
                    b.Pressed <- true
                    b.Act()
                | None -> ()

    override _.OnMouseMove(e: MouseEventArgs) =
        if sliderDrag then setSliderFromX e.X

    override _.OnMouseUp(_e: MouseEventArgs) =
        sliderDrag <- false
        releaseAll ()

    // Don't leave the robot driving if the window loses focus mid-press.
    override _.OnDeactivate(e: EventArgs) =
        base.OnDeactivate e
        down.Clear()
        sliderDrag <- false
        releaseAll ()

    override _.OnFormClosed(e: FormClosedEventArgs) =
        base.OnFormClosed e
        saveSettings ()
        link.StopNow()
        (link :> IDisposable).Dispose()

    // ---- drawing ----

    override _.OnPaint(e: PaintEventArgs) =
        let g = e.Graphics
        g.SmoothingMode <- SmoothingMode.AntiAlias
        g.TextRenderingHint <- TextRenderingHint.ClearTypeGridFit
        g.Clear(Theme.bg)

        let t = link.Telemetry()
        let ok = link.Ok
        let get k = Map.tryFind k t |> Option.defaultValue "--"
        let centre = StringAlignment.Center
        let near = StringAlignment.Near

        let drawButton (b: Btn) =
            let on = b.On() || b.Pressed
            Draw.panel g b.Rect (if on then Theme.accentBg else Theme.panel) (if on then Theme.accent else Theme.border) 2f 8
            Draw.str
                g
                (b.Label())
                fonts.Med
                (if on then Theme.accent else Theme.text)
                (b.Rect.X + b.Rect.Width / 2)
                (b.Rect.Y + b.Rect.Height / 2)
                centre
                centre

        // ---- left column: controls ----
        Draw.str g "MILA" fonts.Big Theme.accent 210 28 centre centre
        let sub =
            sprintf "%s:%d   %s%s" host port (if ok then "connected" else "OFFLINE") (if padIndex >= 0 then "   [gamepad]" else "")
        Draw.str g sub fonts.Sml (if ok then Theme.green else Theme.red) 210 54 centre centre

        for b in buttons do
            if b.Show() && b.Rect.Left < 420 then drawButton b

        // speed slider
        use trackBrush = new SolidBrush(Theme.border)
        g.FillRectangle(trackBrush, SL_L, SL_Y - 3, SL_R - SL_L, 6)
        let kx = SL_L + (speedPct - 25) * (SL_R - SL_L) / 75
        use fillBrush = new SolidBrush(Theme.accent)
        g.FillRectangle(fillBrush, SL_L, SL_Y - 3, kx - SL_L, 6)
        use knobBrush = new SolidBrush(if sliderDrag then Theme.accent else Theme.text)
        use knobPen = new Pen(Theme.accent, 2f)
        g.FillEllipse(knobBrush, kx - 10, SL_Y - 10, 20, 20)
        g.DrawEllipse(knobPen, kx - 10, SL_Y - 10, 20, 20)
        Draw.str g (sprintf "SPEED %d%%" speedPct) fonts.Sml Theme.dim ((SL_L + SL_R) / 2) 156 centre centre

        let sy = 400
        let colw = (420 - 50) / 2
        let c2 = 30 + colw
        let tile x y label value = Draw.tile g fonts x y colw label value
        tile 20 sy "FRONT CM" (get "dist")
        tile c2 sy "LEFT CM" (get "left")
        tile 20 (sy + 54) "RIGHT CM" (get "right")
        tile c2 (sy + 54) "LAST CMD" (get "cmd")
        tile 20 (sy + 108) "LAST IR" (get "ir")
        tile c2 (sy + 108) "TEMP C" (get "temp")
        tile 20 (sy + 162) "HUMIDITY %" (get "hum")
        tile c2 (sy + 162) ("IP  " + get "ip") (if get "fleet" = "1" then "FLEET (joined NORA)" else "STANDALONE AP")
        tile 20 (sy + 216) "SPEED %" (get "speed")
        tile c2 (sy + 216) "GUARD" (if get "guard" = "1" then "ACTIVE" else "clear")

        Draw.str g "1/2/3 mode - WASD/QAED drive - space stop - esc quit" fonts.Sml Theme.dim 210 (H - 18) centre centre

        // ---- right column: radar, graphs, export, discovery ----
        Draw.radar g fonts (Rectangle(440, 20, 380, 242)) (Json.num t "left") (Json.num t "dist") (Json.num t "right") ok

        let hist = link.Tail 150
        let last f = if List.isEmpty hist then nan else f (List.last hist)
        let dists = hist |> List.map (fun s -> s.Dist)
        let dmax = dists |> List.filter (fun v -> not (Double.IsNaN v)) |> List.fold max 100.0
        Draw.graph
            g
            fonts
            (Rectangle(440, 272, 380, 120))
            "FRONT DISTANCE (LAST ~60 S)"
            [ (Theme.accent, dists) ]
            0.0
            dmax
            [ Draw.fmt (last (fun s -> s.Dist)) "0.0" + " cm" ]
        Draw.graph
            g
            fonts
            (Rectangle(440, 402, 380, 120))
            "TEMP / HUMIDITY"
            [ (Theme.accent, hist |> List.map (fun s -> s.Temp)); (Theme.green, hist |> List.map (fun s -> s.Hum)) ]
            0.0
            0.0
            [ Draw.fmt (last (fun s -> s.Temp)) "0.0" + " C"; Draw.fmt (last (fun s -> s.Hum)) "0.0" + " %" ]

        for b in buttons do
            if b.Show() && b.Rect.Left >= 420 then drawButton b
        Draw.str g exportMsg fonts.Sml Theme.dim 632 556 near near

        let status =
            if scanner.Scanning then "looking for MILA on your network..."
            elif scanner.Ever then (if List.isEmpty (scanner.Found()) then "no robots found" else "click an address to connect")
            else "scan to find MILA (or use --host auto)"
        Draw.str g (sprintf "ROBOT  %s:%d" host port) fonts.Sml Theme.dim 440 600 near near
        Draw.str g status fonts.Sml Theme.dim 600 632 near near

module Program =

    [<EntryPoint; STAThread>]
    let main argv =
        let rec parse (settings: Settings, auto: bool) (args: string list) =
            match args with
            | "--host" :: "auto" :: rest -> parse (settings, true) rest
            | "--host" :: h :: rest -> parse ({ settings with Host = h }, auto) rest
            | "--port" :: p :: rest ->
                match Int32.TryParse(p) with
                | true, v -> parse ({ settings with Port = v }, auto) rest
                | _ -> parse (settings, auto) rest
            | _ :: rest -> parse (settings, auto) rest
            | [] -> (settings, auto)

        let settings, auto = parse (Settings.load (), false) (List.ofArray argv)
        Application.EnableVisualStyles()
        Application.Run(new MainForm(settings, auto))
        0
