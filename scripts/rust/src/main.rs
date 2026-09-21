// MILA WiFi controller (egui/eframe) — Rust port of mila_controller.py.
// Mirrors the onboard web dashboard.
//
// USAGE
//   mila_controller
//   mila_controller --host 192.168.4.1 --port 5010
//
//   If MILA joined NORA's network (fleet mode) instead of hosting her own
//   AP, she won't be at 192.168.4.1 anymore — check NORA's dashboard/fleet
//   registry (http://192.168.4.1:5000/robots) for MILA's actual IP and pass
//   it with --host.
//
// BUILD
//   cargo build --release      ->  target/release/mila_controller(.exe)
//
// CONTROLS
//   1 / 2 / 3     WASD / TANK / OBSTACLE mode
//   WASD mode:    Arrow keys or WASD to drive, Space to stop
//   TANK mode:    Q/A = left track fwd/back, E/D = right track fwd/back
//   Esc           quit
//   Everything is also clickable with the mouse.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui::{
    self, Align2, Color32, Event, FontId, Key, Painter, Pos2, Rect, Rounding, Sense, Stroke, Vec2,
};
use std::collections::HashMap;
use std::sync::{mpsc, Arc, Mutex};
use std::thread;
use std::time::Duration;

// ----------------------------------------------------------------------------
// Transport
// ----------------------------------------------------------------------------

#[derive(Default)]
struct Shared {
    ok: bool,
    data: HashMap<String, String>,
}

fn value_to_string(v: &serde_json::Value) -> String {
    match v {
        serde_json::Value::String(s) => s.clone(),
        serde_json::Value::Number(n) => n.to_string(),
        serde_json::Value::Bool(b) => (if *b { "1" } else { "0" }).to_string(),
        other => other.to_string(),
    }
}

struct Link {
    base: String,
    agent: ureq::Agent,
    tx: mpsc::Sender<String>,
    shared: Arc<Mutex<Shared>>,
}

impl Link {
    fn new(host: &str, port: u16) -> Self {
        let base = format!("http://{}:{}", host, port);
        let agent = ureq::AgentBuilder::new()
            .timeout(Duration::from_secs(1))
            .build();
        let shared = Arc::new(Mutex::new(Shared::default()));

        // Status poller.
        {
            let (agent, base, shared) = (agent.clone(), base.clone(), shared.clone());
            thread::spawn(move || loop {
                let parsed = agent
                    .get(&format!("{}/status", base))
                    .call()
                    .ok()
                    .and_then(|r| r.into_json::<serde_json::Value>().ok());
                {
                    let mut s = shared.lock().unwrap();
                    match parsed {
                        Some(serde_json::Value::Object(m)) => {
                            s.data = m
                                .iter()
                                .map(|(k, v)| (k.clone(), value_to_string(v)))
                                .collect();
                            s.ok = true;
                        }
                        _ => s.ok = false,
                    }
                }
                thread::sleep(Duration::from_millis(400));
            });
        }

        // One worker keeps commands in order (a STOP must never overtake the
        // FORWARD that preceded it).
        let (tx, rx) = mpsc::channel::<String>();
        {
            let (agent, base) = (agent.clone(), base.clone());
            thread::spawn(move || {
                while let Ok(path) = rx.recv() {
                    if agent.get(&format!("{}{}", base, path)).call().is_err() {
                        // Offline: drop the backlog rather than replaying stale
                        // drive commands when the link comes back.
                        while rx.try_recv().is_ok() {}
                    }
                }
            });
        }

        Link { base, agent, tx, shared }
    }

    fn cmd(&self, action: &str) {
        let _ = self.tx.send(format!("/cmd?v={}", action));
    }

    fn mode(&self, m: &str) {
        let _ = self.tx.send(format!("/mode?v={}", m));
    }

    /// Synchronous STOP for shutdown, when the worker thread may not get to run.
    fn stop_now(&self) {
        let _ = self.agent.get(&format!("{}/cmd?v=STOP", self.base)).call();
    }
}

// ----------------------------------------------------------------------------
// UI — same dark/cyan palette as the onboard dashboard
// ----------------------------------------------------------------------------

const BG: Color32 = Color32::from_rgb(10, 10, 15);
const PANEL: Color32 = Color32::from_rgb(18, 18, 26);
const BORDER: Color32 = Color32::from_rgb(30, 30, 46);
const ACCENT: Color32 = Color32::from_rgb(0, 212, 255);
const RED: Color32 = Color32::from_rgb(255, 68, 68);
const GREEN: Color32 = Color32::from_rgb(0, 255, 136);
const TEXT: Color32 = Color32::from_rgb(224, 224, 240);
const DIM: Color32 = Color32::from_rgb(102, 102, 136);
const ACCENT_BG: Color32 = Color32::from_rgb(14, 34, 46);

const W: f32 = 420.0;
const H: f32 = 720.0;

#[derive(Clone, Copy)]
enum Act {
    Mode(&'static str),
    Speed(i32),
    Drive(&'static str),
    Stop,
}

const ALWAYS: u8 = 0;
const WASD_ONLY: u8 = 1;
const TANK_ONLY: u8 = 2;

struct Btn {
    rect: Rect,
    label: &'static str,
    group: u8,
    act: Act,
}

fn btn(x: f32, y: f32, w: f32, h: f32, label: &'static str, group: u8, act: Act) -> Btn {
    Btn { rect: Rect::from_min_size(Pos2::new(x, y), Vec2::new(w, h)), label, group, act }
}

fn build_buttons() -> Vec<Btn> {
    let (cx, cy, bs, gap) = (W / 2.0, 280.0, 70.0, 8.0);
    let ty = 280.0;
    vec![
        btn(20.0, 100.0, 120.0, 40.0, "WASD", ALWAYS, Act::Mode("wasd")),
        btn(150.0, 100.0, 120.0, 40.0, "TANK", ALWAYS, Act::Mode("tank")),
        btn(280.0, 100.0, 120.0, 40.0, "OBSTACLE", ALWAYS, Act::Mode("obstacle")),
        // Speed presets mirror the firmware's speedPresets[] (100/75/50/25).
        btn(20.0, 150.0, 180.0, 40.0, "SPD -", ALWAYS, Act::Speed(-25)),
        btn(220.0, 150.0, 180.0, 40.0, "SPD +", ALWAYS, Act::Speed(25)),
        // WASD d-pad
        btn(cx - bs / 2.0, cy - bs - gap, bs, bs, "^", WASD_ONLY, Act::Drive("FORWARD")),
        btn(cx - bs / 2.0, cy + gap, bs, bs, "v", WASD_ONLY, Act::Drive("BACKWARD")),
        btn(cx - bs - gap - bs / 2.0 - gap, cy - bs / 2.0, bs, bs, "<", WASD_ONLY, Act::Drive("LEFT")),
        btn(cx + gap + bs / 2.0 + gap, cy - bs / 2.0, bs, bs, ">", WASD_ONLY, Act::Drive("RIGHT")),
        btn(cx - bs / 2.0, cy - bs / 2.0, bs, bs, "STOP", WASD_ONLY, Act::Stop),
        // TANK tracks
        btn(60.0, ty - 40.0, 90.0, 50.0, "Q\nL-FWD", TANK_ONLY, Act::Drive("L_FWD")),
        btn(60.0, ty + 40.0, 90.0, 50.0, "A\nL-BWD", TANK_ONLY, Act::Drive("L_BWD")),
        btn(W - 150.0, ty - 40.0, 90.0, 50.0, "E\nR-FWD", TANK_ONLY, Act::Drive("R_FWD")),
        btn(W - 150.0, ty + 40.0, 90.0, 50.0, "D\nR-BWD", TANK_ONLY, Act::Drive("R_BWD")),
    ]
}

fn visible(group: u8, mode: &str) -> bool {
    group == ALWAYS || (group == WASD_ONLY && mode == "wasd") || (group == TANK_ONLY && mode == "tank")
}

fn key_drive(mode: &str, k: Key) -> Option<&'static str> {
    match (mode, k) {
        ("wasd", Key::ArrowUp) | ("wasd", Key::W) => Some("FORWARD"),
        ("wasd", Key::ArrowDown) | ("wasd", Key::S) => Some("BACKWARD"),
        ("wasd", Key::ArrowLeft) | ("wasd", Key::A) => Some("LEFT"),
        ("wasd", Key::ArrowRight) | ("wasd", Key::D) => Some("RIGHT"),
        ("tank", Key::Q) => Some("L_FWD"),
        ("tank", Key::A) => Some("L_BWD"),
        ("tank", Key::E) => Some("R_FWD"),
        ("tank", Key::D) => Some("R_BWD"),
        _ => None,
    }
}

fn tile(p: &Painter, x: f32, y: f32, w: f32, label: &str, value: &str) {
    let r = Rect::from_min_size(Pos2::new(x, y), Vec2::new(w, 46.0));
    p.rect(r, Rounding::same(6.0), PANEL, Stroke::new(1.0, BORDER));
    p.text(Pos2::new(x + 8.0, y + 6.0), Align2::LEFT_TOP, label, FontId::monospace(12.0), DIM);
    p.text(Pos2::new(x + 8.0, y + 22.0), Align2::LEFT_TOP, value, FontId::monospace(15.0), ACCENT);
}

struct App {
    link: Link,
    host: String,
    port: u16,
    mode: String, // mirrors the firmware's WASD boot default
    active_drive: Option<&'static str>,
    speed: i32,
    btns: Vec<Btn>,
    was_down: Vec<bool>,
}

impl App {
    fn new(host: String, port: u16) -> Self {
        let btns = build_buttons();
        let n = btns.len();
        App {
            link: Link::new(&host, port),
            host,
            port,
            mode: "wasd".into(),
            active_drive: None,
            speed: 100,
            btns,
            was_down: vec![false; n],
        }
    }

    fn start_drive(&mut self, a: &'static str) {
        self.active_drive = Some(a);
        self.link.cmd(a);
    }

    fn stop_drive(&mut self) {
        if self.active_drive.take().is_some() {
            self.link.cmd("STOP");
        }
    }

    fn run(&mut self, act: Act) {
        match act {
            Act::Mode(m) => {
                self.mode = m.to_string();
                self.link.mode(m);
            }
            Act::Speed(d) => {
                self.speed = (self.speed + d).clamp(25, 100);
                self.link.cmd(&format!("SPEED:{}", self.speed));
            }
            Act::Drive(a) => self.start_drive(a),
            Act::Stop => self.link.cmd("STOP"),
        }
    }

    fn handle_keys(&mut self, ctx: &egui::Context) {
        let events = ctx.input(|i| i.events.clone());
        for ev in events {
            if let Event::Key { key, pressed, repeat, .. } = ev {
                if repeat {
                    continue; // ignore OS auto-repeat
                }
                if pressed {
                    match key {
                        Key::Escape => ctx.send_viewport_cmd(egui::ViewportCommand::Close),
                        Key::Num1 => self.run(Act::Mode("wasd")),
                        Key::Num2 => self.run(Act::Mode("tank")),
                        Key::Num3 => self.run(Act::Mode("obstacle")),
                        Key::Space => self.stop_drive(),
                        k => {
                            if let Some(a) = key_drive(&self.mode, k) {
                                self.start_drive(a);
                            }
                        }
                    }
                } else if let Some(a) = key_drive(&self.mode, key) {
                    if self.active_drive == Some(a) {
                        self.stop_drive();
                    }
                }
            }
        }
        // Don't leave the robot driving if the window loses focus mid-press.
        if !ctx.input(|i| i.focused) {
            self.stop_drive();
        }
    }

    fn draw(&mut self, ui: &mut egui::Ui) {
        let origin = ui.max_rect().min.to_vec2();
        let painter = ui.painter().clone();
        let (ok, t) = {
            let s = self.link.shared.lock().unwrap();
            (s.ok, s.data.clone())
        };
        if let Some(v) = t.get("speed").and_then(|v| v.parse::<f64>().ok()) {
            self.speed = v as i32; // keep in sync with IR remote / dashboard changes
        }
        let get = |k: &str| t.get(k).cloned().unwrap_or_else(|| "--".to_string());
        let at = |x: f32, y: f32| Pos2::new(x, y) + origin;

        painter.text(at(W / 2.0, 28.0), Align2::CENTER_CENTER, "MILA", FontId::monospace(26.0), ACCENT);
        painter.text(
            at(W / 2.0, 54.0),
            Align2::CENTER_CENTER,
            format!("{}:{}   {}", self.host, self.port, if ok { "connected" } else { "OFFLINE" }),
            FontId::monospace(12.0),
            if ok { GREEN } else { RED },
        );

        for i in 0..self.btns.len() {
            let (rect, label, group, act) = {
                let b = &self.btns[i];
                (b.rect.translate(origin), b.label, b.group, b.act)
            };
            if !visible(group, &self.mode) {
                self.was_down[i] = false;
                continue;
            }
            let resp = ui.interact(rect, ui.id().with(i), Sense::click_and_drag());
            let is_down = resp.is_pointer_button_down_on();
            if is_down && !self.was_down[i] {
                self.run(act);
            } else if !is_down && self.was_down[i] {
                self.stop_drive();
            }
            self.was_down[i] = is_down;

            let selected = matches!(act, Act::Mode(m) if m == self.mode);
            let on = is_down || selected;
            painter.rect(
                rect,
                Rounding::same(8.0),
                if on { ACCENT_BG } else { PANEL },
                Stroke::new(2.0, if on { ACCENT } else { BORDER }),
            );
            painter.text(
                rect.center(),
                Align2::CENTER_CENTER,
                label,
                FontId::monospace(15.0),
                if on { ACCENT } else { TEXT },
            );
        }

        let (sy, colw) = (400.0, (W - 50.0) / 2.0);
        let c2 = 30.0 + colw;
        let tl = |x: f32, y: f32, label: &str, value: &str| tile(&painter, x + origin.x, y + origin.y, colw, label, value);
        tl(20.0, sy, "FRONT CM", &get("dist"));
        tl(c2, sy, "LEFT CM", &get("left"));
        tl(20.0, sy + 54.0, "RIGHT CM", &get("right"));
        tl(c2, sy + 54.0, "LAST CMD", &get("cmd"));
        tl(20.0, sy + 108.0, "LAST IR", &get("ir"));
        tl(c2, sy + 108.0, "TEMP C", &get("temp"));
        tl(20.0, sy + 162.0, "HUMIDITY %", &get("hum"));
        let fleet = if get("fleet") == "1" { "FLEET (joined NORA)" } else { "STANDALONE AP" };
        tl(c2, sy + 162.0, &format!("IP  {}", get("ip")), fleet);
        tl(20.0, sy + 216.0, "SPEED %", &get("speed"));
        tl(c2, sy + 216.0, "GUARD", if get("guard") == "1" { "ACTIVE" } else { "clear" });

        painter.text(
            at(W / 2.0, H - 18.0),
            Align2::CENTER_CENTER,
            "1/2/3 mode  -  arrows/WASD or QAED drive  -  space stop  -  esc quit",
            FontId::monospace(11.0),
            DIM,
        );
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.handle_keys(ctx);
        egui::CentralPanel::default()
            .frame(egui::Frame::none().fill(BG))
            .show(ctx, |ui| self.draw(ui));
        ctx.request_repaint_after(Duration::from_millis(16));
    }

    fn on_exit(&mut self, _gl: Option<&eframe::glow::Context>) {
        self.link.stop_now();
    }
}

fn main() -> eframe::Result<()> {
    let mut host = "192.168.4.1".to_string();
    let mut port: u16 = 5010;
    let args: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--host" if i + 1 < args.len() => {
                host = args[i + 1].clone();
                i += 1;
            }
            "--port" if i + 1 < args.len() => {
                port = args[i + 1].parse().unwrap_or(port);
                i += 1;
            }
            _ => {}
        }
        i += 1;
    }

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([W, H])
            .with_resizable(false),
        ..Default::default()
    };
    eframe::run_native(
        "MILA Control",
        options,
        Box::new(move |_cc| Ok(Box::new(App::new(host, port)))),
    )
}
