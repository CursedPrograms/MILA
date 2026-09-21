namespace Mila

open System
open System.Collections.Concurrent
open System.Collections.Generic
open System.Net.Http
open System.Net.NetworkInformation
open System.Net.Sockets
open System.Text.RegularExpressions
open System.Threading

/// Talks to MILA's ESP8266 web server (/status, /cmd, /mode).
type WifiLink(initialHost: string, initialPort: int) =
    let lk = obj ()
    let mutable host = initialHost
    let mutable port = initialPort
    let mutable data : Map<string, string> = Map.empty
    let history = List<Sample>()
    let http = new HttpClient(Timeout = TimeSpan.FromSeconds(1.0))
    let queue = new BlockingCollection<string>()
    [<VolatileField>]
    let mutable ok = false
    [<VolatileField>]
    let mutable running = true

    let baseUrl () = lock lk (fun () -> sprintf "http://%s:%d" host port)

    let poll () =
        while running do
            try
                let body = http.GetStringAsync(baseUrl () + "/status").Result
                let parsed = Json.parseFlat body
                lock lk (fun () ->
                    data <- parsed
                    history.Add(Sample.ofStatus parsed)
                    if history.Count > 200000 then history.RemoveRange(0, 1000))
                ok <- true
            with _ ->
                ok <- false
            Thread.Sleep 400

    // One worker keeps commands in order (a STOP must never overtake the FORWARD before it).
    let work () =
        for path in queue.GetConsumingEnumerable() do
            try
                http.GetStringAsync(baseUrl () + path).Wait()
            with _ ->
                // Offline: drop the backlog rather than replaying stale drive commands when the link returns.
                let mutable dropped : string = null
                while queue.TryTake(&dropped) do ()

    do
        for f in [ poll; work ] do
            let t = Thread(ThreadStart(f), IsBackground = true)
            t.Start()

    member _.Ok = ok

    member _.Telemetry() : Map<string, string> = lock lk (fun () -> data)

    member _.Retarget(newHost: string, newPort: int) =
        lock lk (fun () ->
            host <- newHost
            port <- newPort
            data <- Map.empty
            ok <- false)

    member _.Tail(n: int) : Sample list =
        lock lk (fun () -> history |> Seq.skip (max 0 (history.Count - n)) |> List.ofSeq)

    member _.All() : Sample list = lock lk (fun () -> List.ofSeq history)

    member _.Cmd(action: string) = queue.Add("/cmd?v=" + action)

    member _.Mode(m: string) = queue.Add("/mode?v=" + m)

    /// Synchronous STOP for shutdown, when the worker may not get to run.
    member _.StopNow() =
        try http.GetStringAsync(baseUrl () + "/cmd?v=STOP").Wait() with _ -> ()

    interface IDisposable with
        member _.Dispose() =
            running <- false
            queue.CompleteAdding()
            http.Dispose()

/// Finding MILA on the network.
module Net =

    /// Non-blocking connect with a hard timeout, so sweeping a /24 stays fast.
    let tcpOpen (host: string) (port: int) (ms: int) : bool =
        try
            use c = new TcpClient()
            let r = c.BeginConnect(host, port, null, null)
            if not (r.AsyncWaitHandle.WaitOne(ms: int)) then
                false
            else
                c.EndConnect r
                c.Connected
        with _ ->
            false

    let httpGet (url: string) (timeoutMs: int) : string option =
        try
            use c = new HttpClient(Timeout = TimeSpan.FromMilliseconds(float timeoutMs))
            Some(c.GetStringAsync(url).Result)
        with _ ->
            None

    /// Does something at host:port look like a MILA?
    let probe (host: string) (port: int) : bool =
        tcpOpen host port 300
        && (match httpGet (sprintf "http://%s:%d/status" host port) 800 with
            | Some body -> body.Contains("\"mode\"") && body.Contains("\"dist\"")
            | None -> false)

    let localSubnets () : string list =
        try
            NetworkInterface.GetAllNetworkInterfaces()
            |> Seq.filter (fun ni ->
                ni.OperationalStatus = OperationalStatus.Up
                && ni.NetworkInterfaceType <> NetworkInterfaceType.Loopback)
            |> Seq.collect (fun ni -> ni.GetIPProperties().UnicastAddresses |> Seq.map (fun u -> u.Address))
            |> Seq.filter (fun a -> a.AddressFamily = AddressFamily.InterNetwork)
            |> Seq.map (fun a -> a.GetAddressBytes())
            |> Seq.filter (fun b -> b.[0] <> 127uy && not (b.[0] = 169uy && b.[1] = 254uy))
            |> Seq.map (fun b -> sprintf "%d.%d.%d." b.[0] b.[1] b.[2])
            |> Seq.distinct
            |> List.ofSeq
        with _ ->
            []

    /// Everything worth probing, most likely first.
    let candidates () : string list =
        // Robots registered with NORA (best effort: pull every IPv4 out of the reply).
        let registry =
            match httpGet "http://192.168.4.1:5000/robots" 400 with
            | Some body ->
                Regex.Matches(body, @"(\d{1,3}\.){3}\d{1,3}")
                |> Seq.cast<Match>
                |> Seq.map (fun m -> m.Value)
                |> List.ofSeq
            | None -> []
        let sweep =
            [ for pre in localSubnets () @ [ "192.168.4." ] do
                  for i in 1..254 -> pre + string i ]
        // 127.0.0.1 = a simulator on this machine (sim/MockMila.cs); mila.local = the firmware's mDNS name.
        registry @ [ "127.0.0.1"; "mila.local"; "192.168.4.1" ] @ sweep |> List.distinct

/// Background network scan; the UI polls its state every frame.
type Scanner() =
    let lk = obj ()
    let found = List<string>()
    let finished = ref 0
    [<VolatileField>]
    let mutable scanning = false
    [<VolatileField>]
    let mutable autoIp : string = null
    let mutable ever = false
    let mutable total = 0

    member _.Scanning = scanning
    member _.Ever = ever

    member _.Percent = if total > 0 then 100 * finished.Value / total else 0

    member _.Found() : string list = lock lk (fun () -> List.ofSeq found)

    /// The first robot found by an `--host auto` scan, once, so the UI thread can connect to it.
    member _.TakeAuto() : string =
        let ip = autoIp
        autoIp <- null
        ip

    member _.Start(port: int, auto: bool) =
        if not scanning then
            scanning <- true
            ever <- true
            finished.Value <- 0
            total <- 0
            autoIp <- null
            lock lk (fun () -> found.Clear())

            let run () =
                let cands = Net.candidates () |> Array.ofList
                total <- cands.Length
                let next = ref (-1)

                let worker () =
                    let mutable go = true
                    while go do
                        let i = Interlocked.Increment(&next.contents)
                        if i >= cands.Length then
                            go <- false
                        else
                            if Net.probe cands.[i] port then
                                lock lk (fun () -> found.Add(cands.[i]))
                                if auto && isNull autoIp then autoIp <- cands.[i]
                            Interlocked.Increment(&finished.contents) |> ignore

                let pool = [ for _ in 1..64 -> Thread(ThreadStart(worker), IsBackground = true) ]
                pool |> List.iter (fun t -> t.Start())
                pool |> List.iter (fun t -> t.Join())
                scanning <- false

            Thread(ThreadStart(run), IsBackground = true).Start()
