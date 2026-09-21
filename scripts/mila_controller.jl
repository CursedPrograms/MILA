#!/usr/bin/env julia
# MILA WiFi controller (GLMakie) — Julia port of mila_controller.py.
# Mirrors the onboard web dashboard.
#
# USAGE
#   julia mila_controller.jl
#   julia mila_controller.jl --host 192.168.4.1 --port 5010
#
#   If MILA joined NORA's network (fleet mode) instead of hosting her own
#   AP, she won't be at 192.168.4.1 anymore — check NORA's dashboard/fleet
#   registry (http://192.168.4.1:5000/robots) for MILA's actual IP and pass
#   it with --host.
#
# INSTALL (first run precompiles GLMakie, which takes a few minutes)
#   julia -e 'using Pkg; Pkg.add(["GLMakie", "HTTP", "JSON"])'
#
# CONTROLS
#   1 / 2 / 3     WASD / TANK / OBSTACLE mode
#   WASD mode:    Arrow keys or WASD to drive, Space to stop
#   TANK mode:    Q/A = left track fwd/back, E/D = right track fwd/back
#   Esc           quit
#   Everything is also clickable with the mouse.

using GLMakie
using HTTP
using JSON

# ----------------------------------------------------------------------------
# Palette — same dark/cyan look as the onboard dashboard
# ----------------------------------------------------------------------------

rgb(r, g, b) = RGBf(r / 255, g / 255, b / 255)

const BG        = rgb(10, 10, 15)
const PANEL     = rgb(18, 18, 26)
const BORDER    = rgb(30, 30, 46)
const ACCENT    = rgb(0, 212, 255)
const RED       = rgb(255, 68, 68)
const GREEN     = rgb(0, 255, 136)
const TEXT      = rgb(224, 224, 240)
const DIM       = rgb(102, 102, 136)
const ACCENT_BG = rgb(14, 34, 46)

const W, H = 420, 720

# ----------------------------------------------------------------------------
# Transport
# ----------------------------------------------------------------------------

struct WifiLink
    base::String
    online::Observable{Bool}
    telemetry::Observable{Dict{String,Any}}
    running::Ref{Bool}
end

function WifiLink(host::AbstractString, port::Integer)
    link = WifiLink("http://$host:$port", Observable(false),
                    Observable(Dict{String,Any}()), Ref(true))
    @async while link.running[]
        try
            r = HTTP.get(link.base * "/status"; readtimeout = 1, connect_timeout = 1,
                         retry = false)
            link.telemetry[] = JSON.parse(String(r.body))
            link.online[] = true
        catch
            link.online[] = false
        end
        sleep(0.4)
    end
    return link
end

# Fire-and-forget GET; returns the task so callers can wait when they must.
function get_async(link::WifiLink, path::AbstractString)
    @async try
        HTTP.get(link.base * path; readtimeout = 1, connect_timeout = 1, retry = false,
                 status_exception = false)
    catch
    end
end

cmd(link::WifiLink, action) = get_async(link, "/cmd?v=$action")
setmode(link::WifiLink, m) = get_async(link, "/mode?v=$m")

# ----------------------------------------------------------------------------
# UI
# ----------------------------------------------------------------------------

mutable struct Btn
    x::Int
    y::Int
    w::Int
    h::Int
    label::String
    group::Symbol            # :always, :wasd or :tank
    action::Function
    active::Observable{Bool}
    pressed::Observable{Bool}
end

hit(b::Btn, p) = b.x <= p[1] <= b.x + b.w && b.y <= p[2] <= b.y + b.h

tv(t, k) = haskey(t, k) ? string(t[k]) : "--"

function main(args = ARGS)
    host, port = "192.168.4.1", 5010
    i = 1
    while i <= length(args)
        if args[i] == "--host" && i < length(args)
            host = args[i+1]; i += 2
        elseif args[i] == "--port" && i < length(args)
            port = parse(Int, args[i+1]); i += 2
        else
            i += 1
        end
    end

    link = WifiLink(host, port)

    mode = Observable("wasd")              # mirrors the firmware's WASD boot default
    active_drive = Ref("")
    speed_pct = Ref(100)

    function start_drive(action)
        active_drive[] = action
        cmd(link, action)
    end
    function stop_drive()
        if !isempty(active_drive[])
            active_drive[] = ""
            cmd(link, "STOP")
        end
    end
    function set_mode(m)
        mode[] = m
        setmode(link, m)
    end
    # Speed presets mirror the firmware's speedPresets[] (100/75/50/25).
    function adjust_speed(delta)
        speed_pct[] = clamp(speed_pct[] + delta, 25, 100)
        cmd(link, "SPEED:$(speed_pct[])")
    end

    # ---- figure ----
    fig = Figure(size = (W, H), backgroundcolor = BG, figure_padding = 0)
    ax = Axis(fig[1, 1]; backgroundcolor = BG, yreversed = true, limits = (0, W, 0, H))
    hidedecorations!(ax)
    hidespines!(ax)
    for name in (:rectanglezoom, :scrollzoom, :dragpan, :limitreset)
        deregister_interaction!(ax, name)
    end

    text!(ax, Point2f(W / 2, 28); text = "MILA", fontsize = 26, font = :bold,
          color = ACCENT, align = (:center, :center))
    status_txt = lift(link.online) do ok
        "$host:$port   " * (ok ? "connected" : "OFFLINE")
    end
    status_col = lift(ok -> ok ? GREEN : RED, link.online)
    text!(ax, Point2f(W / 2, 54); text = status_txt, fontsize = 12, color = status_col,
          align = (:center, :center))

    # ---- buttons ----
    btns = Btn[]
    function add!(x, y, w, h, label, action; group = :always, active = false)
        b = Btn(x, y, w, h, label, group, action, Observable(active), Observable(false))
        push!(btns, b)

        vis = group === :always ? Observable(true) : lift(m -> m == String(group), mode)
        on_state = lift((a, p) -> a || p, b.active, b.pressed)
        fill = lift(s -> s ? ACCENT_BG : PANEL, on_state)
        edge = lift(s -> s ? ACCENT : BORDER, on_state)
        txtc = lift(s -> s ? ACCENT : TEXT, on_state)

        poly!(ax, Rect2f(x, y, w, h); color = fill, strokecolor = edge, strokewidth = 2,
              visible = vis)
        text!(ax, Point2f(x + w / 2, y + h / 2); text = label, fontsize = 15, font = :bold,
              color = txtc, align = (:center, :center), visible = vis)
        return b
    end

    mode_btns = [
        add!(20, 100, 120, 40, "WASD", () -> set_mode("wasd"); active = true),
        add!(150, 100, 120, 40, "TANK", () -> set_mode("tank")),
        add!(280, 100, 120, 40, "OBSTACLE", () -> set_mode("obstacle")),
    ]
    on(mode) do m
        for (b, name) in zip(mode_btns, ("wasd", "tank", "obstacle"))
            b.active[] = (name == m)
        end
    end

    add!(20, 150, 180, 40, "SPD -", () -> adjust_speed(-25))
    add!(220, 150, 180, 40, "SPD +", () -> adjust_speed(25))

    # WASD d-pad
    cx, cy, bs, gap = W ÷ 2, 280, 70, 8
    add!(cx - bs ÷ 2, cy - bs - gap, bs, bs, "^", () -> start_drive("FORWARD"); group = :wasd)
    add!(cx - bs ÷ 2, cy + gap, bs, bs, "v", () -> start_drive("BACKWARD"); group = :wasd)
    add!(cx - bs - gap - bs ÷ 2 - gap, cy - bs ÷ 2, bs, bs, "<", () -> start_drive("LEFT"); group = :wasd)
    add!(cx + gap + bs ÷ 2 + gap, cy - bs ÷ 2, bs, bs, ">", () -> start_drive("RIGHT"); group = :wasd)
    add!(cx - bs ÷ 2, cy - bs ÷ 2, bs, bs, "STOP", () -> cmd(link, "STOP"); group = :wasd)

    # TANK track buttons
    ty = 280
    add!(60, ty - 40, 90, 50, "Q\nL-FWD", () -> start_drive("L_FWD"); group = :tank)
    add!(60, ty + 40, 90, 50, "A\nL-BWD", () -> start_drive("L_BWD"); group = :tank)
    add!(W - 150, ty - 40, 90, 50, "E\nR-FWD", () -> start_drive("R_FWD"); group = :tank)
    add!(W - 150, ty + 40, 90, 50, "D\nR-BWD", () -> start_drive("R_BWD"); group = :tank)

    is_visible(b) = b.group === :always || String(b.group) == mode[]

    # ---- status tiles ----
    sy, colw = 400, (W - 50) ÷ 2
    c2 = 30 + colw
    function tile!(x, y, label, value)
        poly!(ax, Rect2f(x, y, colw, 46); color = PANEL, strokecolor = BORDER, strokewidth = 1)
        text!(ax, Point2f(x + 8, y + 6); text = label, fontsize = 12, color = DIM,
              align = (:left, :top))
        text!(ax, Point2f(x + 8, y + 22); text = value, fontsize = 15, font = :bold,
              color = ACCENT, align = (:left, :top))
    end
    T = link.telemetry
    tile!(20, sy, "FRONT CM", lift(t -> tv(t, "dist"), T))
    tile!(c2, sy, "LEFT CM", lift(t -> tv(t, "left"), T))
    tile!(20, sy + 54, "RIGHT CM", lift(t -> tv(t, "right"), T))
    tile!(c2, sy + 54, "LAST CMD", lift(t -> tv(t, "cmd"), T))
    tile!(20, sy + 108, "LAST IR", lift(t -> tv(t, "ir"), T))
    tile!(c2, sy + 108, "TEMP C", lift(t -> tv(t, "temp"), T))
    tile!(20, sy + 162, "HUMIDITY %", lift(t -> tv(t, "hum"), T))
    tile!(c2, sy + 162, lift(t -> "IP  " * tv(t, "ip"), T),
          lift(t -> tv(t, "fleet") == "1" ? "FLEET (joined NORA)" : "STANDALONE AP", T))
    tile!(20, sy + 216, "SPEED %", lift(t -> tv(t, "speed"), T))
    tile!(c2, sy + 216, "GUARD", lift(t -> tv(t, "guard") == "1" ? "ACTIVE" : "clear", T))

    text!(ax, Point2f(W / 2, H - 18);
          text = "1/2/3 mode - WASD/QAED drive - space stop - esc quit",
          fontsize = 11, color = DIM, align = (:center, :center))

    # Keep speed_pct in sync with telemetry so it stays correct even if the IR
    # remote or dashboard changed it out from under us.
    on(T) do t
        haskey(t, "speed") || return
        try
            speed_pct[] = trunc(Int, parse(Float64, string(t["speed"])))
        catch
        end
    end

    # ---- input ----
    screen_ref = Ref{Any}(nothing)

    key_wasd = Dict(Keyboard.up => "FORWARD", Keyboard.w => "FORWARD",
                    Keyboard.down => "BACKWARD", Keyboard.s => "BACKWARD",
                    Keyboard.left => "LEFT", Keyboard.a => "LEFT",
                    Keyboard.right => "RIGHT", Keyboard.d => "RIGHT")
    key_tank = Dict(Keyboard.q => "L_FWD", Keyboard.a => "L_BWD",
                    Keyboard.e => "R_FWD", Keyboard.d => "R_BWD")

    on(events(fig).keyboardbutton) do ev
        k = ev.key
        if ev.action == Keyboard.press           # ignores auto-repeat
            if k == Keyboard.escape
                screen_ref[] === nothing || close(screen_ref[])
            elseif k == Keyboard._1
                set_mode("wasd")
            elseif k == Keyboard._2
                set_mode("tank")
            elseif k == Keyboard._3
                set_mode("obstacle")
            elseif k == Keyboard.space
                stop_drive()
            elseif mode[] == "wasd" && haskey(key_wasd, k)
                start_drive(key_wasd[k])
            elseif mode[] == "tank" && haskey(key_tank, k)
                start_drive(key_tank[k])
            end
        elseif ev.action == Keyboard.release
            keymap = mode[] == "wasd" ? key_wasd : mode[] == "tank" ? key_tank : nothing
            if keymap !== nothing && haskey(keymap, k) && active_drive[] == keymap[k]
                stop_drive()
            end
        end
        return
    end

    on(events(fig).mousebutton) do ev
        ev.button == Mouse.left || return
        if ev.action == Mouse.press
            p = mouseposition(ax.scene)
            for b in btns
                if is_visible(b) && hit(b, p)
                    b.pressed[] = true
                    b.action()
                    break
                end
            end
        elseif ev.action == Mouse.release
            for b in btns
                b.pressed[] = false
            end
            stop_drive()
        end
        return
    end

    # Don't leave the robot driving if the window loses focus mid-press.
    on(events(fig).hasfocus) do focused
        if !focused
            for b in btns
                b.pressed[] = false
            end
            stop_drive()
        end
        return
    end

    screen = display(GLMakie.Screen(; title = "MILA Control"), fig)
    screen_ref[] = screen
    wait(screen)

    # Window closed: make sure the robot isn't left driving, then shut down.
    active_drive[] = ""
    try
        wait(cmd(link, "STOP"))
    catch
    end
    link.running[] = false
    return
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
