namespace Mila

open System
open System.Globalization
open System.IO
open System.Text

/// One telemetry reading, kept for the history graphs and CSV export.
type Sample =
    { Time: DateTime
      Dist: float
      Left: float
      Right: float
      Temp: float
      Hum: float
      Speed: float
      Mode: string
      Cmd: string
      Guard: float
      MotorL: float
      MotorR: float }

module Json =

    /// Minimal parser for the firmware's flat /status JSON: {"k":"str","k2":12,...}
    let parseFlat (s: string) : Map<string, string> =
        let n = s.Length
        let i = ref 0

        let readString () =
            let sb = StringBuilder()
            i.Value <- i.Value + 1 // opening quote
            while i.Value < n && s.[i.Value] <> '"' do
                if s.[i.Value] = '\\' && i.Value + 1 < n then i.Value <- i.Value + 1
                sb.Append(s.[i.Value]) |> ignore
                i.Value <- i.Value + 1
            i.Value <- i.Value + 1 // closing quote
            sb.ToString()

        let mutable acc = Map.empty
        while i.Value < n do
            while i.Value < n && s.[i.Value] <> '"' do i.Value <- i.Value + 1
            if i.Value < n then
                let key = readString ()
                while i.Value < n && s.[i.Value] <> ':' do i.Value <- i.Value + 1
                i.Value <- i.Value + 1
                while i.Value < n && (s.[i.Value] = ' ' || s.[i.Value] = '\t') do i.Value <- i.Value + 1
                let value =
                    if i.Value < n && s.[i.Value] = '"' then
                        readString ()
                    else
                        let start = i.Value
                        while i.Value < n && s.[i.Value] <> ',' && s.[i.Value] <> '}' do i.Value <- i.Value + 1
                        s.Substring(start, i.Value - start).Trim()
                acc <- Map.add key value acc
        acc

    /// A numeric field, or NaN when it is missing or not a number (the firmware sends "---" for no reading).
    let num (m: Map<string, string>) (key: string) : float =
        match Map.tryFind key m with
        | Some v ->
            match Double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture) with
            | true, d -> d
            | _ -> nan
        | None -> nan

    let str (m: Map<string, string>) (key: string) : string =
        Map.tryFind key m |> Option.defaultValue ""

module Sample =

    let ofStatus (m: Map<string, string>) : Sample =
        { Time = DateTime.Now
          Dist = Json.num m "dist"
          Left = Json.num m "left"
          Right = Json.num m "right"
          Temp = Json.num m "temp"
          Hum = Json.num m "hum"
          Speed = Json.num m "speed"
          Mode = Json.str m "mode"
          Cmd = Json.str m "cmd"
          Guard = Json.num m "guard"
          MotorL = Json.num m "ml"
          MotorR = Json.num m "mr" }

/// Remembered between runs in %APPDATA%\MILA\controller.ini (the same file the other controllers use).
type Settings =
    { Host: string
      Port: int
      Mode: string
      HasMode: bool }

module Settings =

    let defaults =
        { Host = "192.168.4.1"
          Port = 5010
          Mode = "wasd"
          HasMode = false }

    let private path (create: bool) =
        let dir =
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MILA")
        if create then
            try Directory.CreateDirectory dir |> ignore with _ -> ()
        Path.Combine(dir, "controller.ini")

    let private tryInt (s: string) =
        match Int32.TryParse(s) with
        | true, v -> Some v
        | _ -> None

    let private isMode (v: string) = v = "wasd" || v = "tank" || v = "obstacle"

    let private apply (s: Settings) (line: string) =
        match line.IndexOf '=' with
        | -1 -> s
        | eq ->
            let k = line.Substring(0, eq).Trim()
            let v = line.Substring(eq + 1).Trim()
            match k with
            | "host" when v <> "" -> { s with Host = v }
            | "port" ->
                match tryInt v with
                | Some p when p > 0 -> { s with Port = p }
                | _ -> s
            | "mode" when isMode v -> { s with Mode = v; HasMode = true }
            | _ -> s

    let load () : Settings =
        try
            File.ReadAllLines(path false) |> Array.fold apply defaults
        with _ ->
            defaults

    let save (s: Settings) : unit =
        try
            File.WriteAllText(path true, sprintf "host=%s\nport=%d\nmode=%s\n" s.Host s.Port s.Mode)
        with _ ->
            ()

module Csv =

    let private cell (v: float) =
        if Double.IsNaN v then "" else v.ToString("R", CultureInfo.InvariantCulture)

    let private row (s: Sample) =
        String.Join(
            ",",
            [ s.Time.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture)
              cell s.Dist
              cell s.Left
              cell s.Right
              cell s.Temp
              cell s.Hum
              cell s.Speed
              s.Mode
              s.Cmd
              cell s.Guard
              cell s.MotorL
              cell s.MotorR ]
        )

    /// Writes the whole session to Documents\MILA-telemetry and returns a short message for the UI.
    let export (samples: Sample list) : string =
        if List.isEmpty samples then
            "no samples yet"
        else
            try
                let dir =
                    Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "MILA-telemetry")
                Directory.CreateDirectory dir |> ignore
                let fname =
                    "telemetry_" + DateTime.Now.ToString("yyyyMMdd_HHmmss", CultureInfo.InvariantCulture) + ".csv"
                let header = "timestamp,dist_cm,left_cm,right_cm,temp_c,hum_pct,speed_pct,mode,last_cmd,guard,motor_l,motor_r"
                File.WriteAllLines(Path.Combine(dir, fname), header :: List.map row samples)
                sprintf "saved %d rows: %s" (List.length samples) fname
            with _ ->
                "could not write file"
