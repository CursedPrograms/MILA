namespace Mila

open System
open System.Runtime.InteropServices

[<Struct; StructLayout(LayoutKind.Sequential)>]
type XInputGamepad =
    val mutable Buttons: uint16
    val mutable LeftTrigger: byte
    val mutable RightTrigger: byte
    val mutable ThumbLX: int16
    val mutable ThumbLY: int16
    val mutable ThumbRX: int16
    val mutable ThumbRY: int16

[<Struct; StructLayout(LayoutKind.Sequential)>]
type XInputState =
    val mutable PacketNumber: uint32
    val mutable Gamepad: XInputGamepad

module Native =

    [<DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")>]
    extern int get14(int index, XInputState& state)

    [<DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")>]
    extern int get910(int index, XInputState& state)

    let mutable private dll = 0 // 0 = untried, 1 = xinput1_4, 2 = xinput9_1_0, -1 = unavailable

    /// Reads controller `index`. Returns false if it is not connected (or XInput is missing).
    let getState (index: int, state: byref<XInputState>) : bool =
        if dll = -1 then
            false
        else
            let mutable result = -1
            let mutable handled = false
            if dll <> 2 then
                try
                    result <- get14 (index, &state)
                    dll <- 1
                    handled <- true
                with
                | :? DllNotFoundException
                | :? EntryPointNotFoundException -> dll <- 2
            if not handled then
                try
                    result <- get910 (index, &state)
                    dll <- 2
                with _ ->
                    dll <- -1
            dll <> -1 && result = 0

module Buttons =
    let dpadUp = 0x0001us
    let dpadDown = 0x0002us
    let dpadLeft = 0x0004us
    let dpadRight = 0x0008us
    let leftBumper = 0x0100us
    let rightBumper = 0x0200us
    let a = 0x1000us

/// Turning stick positions into MILA commands. Pure functions, so they are easy to reason about.
module PadMap =

    let deadzone = 0.45

    /// -32768..32767 -> -1.0..1.0
    let axis (v: int16) : float = max (-1.0) (float v / 32767.0)

    let private has (buttons: uint16) (flag: uint16) = (buttons &&& flag) <> 0us

    /// WASD mode: the dominant stick axis (or the D-pad) picks a direction; "" means idle.
    let wasdDirection (lx: float) (ly: float) (buttons: uint16) : string =
        if Math.Sqrt(lx * lx + ly * ly) >= deadzone then
            if abs ly >= abs lx then
                (if ly > 0.0 then "FORWARD" else "BACKWARD")
            else
                (if lx > 0.0 then "RIGHT" else "LEFT")
        elif has buttons Buttons.dpadUp then "FORWARD"
        elif has buttons Buttons.dpadDown then "BACKWARD"
        elif has buttons Buttons.dpadLeft then "LEFT"
        elif has buttons Buttons.dpadRight then "RIGHT"
        else ""

    /// TANK mode: one stick axis per track -> +1 forward, -1 back, 0 idle.
    let track (y: float) : int =
        if y >= deadzone then 1
        elif y <= -deadzone then -1
        else 0

    /// Commands to send when the tracks change from (prevL, prevR) to (l, r), and whether anything is still driving.
    /// STOP is global, so when a track lets go we stop everything and re-issue the track that is still held.
    let tankCommands (prevL: int, prevR: int) (l: int, r: int) : string list * bool =
        if l = 0 && r = 0 then
            ([ "STOP" ], false)
        else
            let released = (prevL <> 0 && l = 0) || (prevR <> 0 && r = 0)
            let cmds =
                [ if released then
                      yield "STOP"
                  if l <> 0 && (released || l <> prevL) then
                      yield (if l > 0 then "L_FWD" else "L_BWD")
                  if r <> 0 && (released || r <> prevR) then
                      yield (if r > 0 then "R_FWD" else "R_BWD") ]
            (cmds, true)
