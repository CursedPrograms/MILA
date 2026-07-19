#!/usr/bin/env python3
"""
MILA WiFi controller (pygame) — mirrors the onboard web dashboard.

USAGE
  python3 mila_controller.py
  python3 mila_controller.py --host 192.168.4.1 --port 5010

  If MILA joined NORA's network (fleet mode) instead of hosting her own
  AP, she won't be at 192.168.4.1 anymore — check NORA's dashboard/fleet
  registry (http://192.168.4.1:5000/robots) for MILA's actual IP and pass
  it with --host.

INSTALL
  pip install pygame requests

CONTROLS
  1 / 2 / 3     WASD / TANK / OBSTACLE mode
  WASD mode:    Arrow keys or WASD to drive, Space to stop
  TANK mode:    Q/A = left track fwd/back, E/D = right track fwd/back
  Esc           quit
  Everything is also clickable with the mouse.
"""

import argparse
import threading
import time

import pygame
import requests

# ----------------------------------------------------------------------------
# Transport
# ----------------------------------------------------------------------------

class WifiLink:
    """Talks to MILA's ESP8266 web server (/cmd, /mode, /status)."""

    def __init__(self, host, port):
        self.base = f"http://{host}:{port}"
        self.ok = False
        self.data = {}
        self._lock = threading.Lock()
        threading.Thread(target=self._poll, daemon=True).start()

    def _get(self, path):
        try:
            requests.get(self.base + path, timeout=1.0)
        except Exception:
            pass

    def _poll(self):
        while True:
            try:
                r = requests.get(self.base + "/status", timeout=1.0)
                with self._lock:
                    self.data = r.json()
                self.ok = True
            except Exception:
                self.ok = False
            time.sleep(0.4)

    def telemetry(self):
        with self._lock:
            return dict(self.data)

    def cmd(self, action):
        threading.Thread(target=self._get, args=(f"/cmd?v={action}",), daemon=True).start()

    def mode(self, m):
        threading.Thread(target=self._get, args=(f"/mode?v={m}",), daemon=True).start()


# ----------------------------------------------------------------------------
# UI — same dark/cyan palette as the onboard dashboard
# ----------------------------------------------------------------------------

BG      = (10, 10, 15)     # --bg
PANEL   = (18, 18, 26)     # --panel
BORDER  = (30, 30, 46)     # --border
ACCENT  = (0, 212, 255)    # --accent
ACCENT2 = (124, 58, 237)   # --accent2
RED     = (255, 68, 68)    # --red
GREEN   = (0, 255, 136)    # --green
TEXT    = (224, 224, 240)  # --text
DIM     = (102, 102, 136)  # --sub
ACCENT_BG = (14, 34, 46)

W, H = 420, 720


class Button:
    def __init__(self, rect, label, cb, font, active=False):
        self.rect = pygame.Rect(rect)
        self.label = label
        self.cb = cb
        self.font = font
        self.active = active
        self.pressed = False

    def draw(self, surf):
        color = ACCENT if (self.active or self.pressed) else BORDER
        bg = ACCENT_BG if (self.active or self.pressed) else PANEL
        pygame.draw.rect(surf, bg, self.rect, border_radius=8)
        pygame.draw.rect(surf, color, self.rect, 2, border_radius=8)
        txt = self.font.render(self.label, True, ACCENT if (self.active or self.pressed) else TEXT)
        surf.blit(txt, txt.get_rect(center=self.rect.center))

    def hit(self, pos):
        return self.rect.collidepoint(pos)


def stat_tile(surf, font_label, font_val, x, y, w, label, value):
    box = pygame.Rect(x, y, w, 46)
    pygame.draw.rect(surf, PANEL, box, border_radius=6)
    pygame.draw.rect(surf, BORDER, box, 1, border_radius=6)
    surf.blit(font_label.render(label, True, DIM), (x + 8, y + 6))
    surf.blit(font_val.render(str(value), True, ACCENT), (x + 8, y + 20))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--port", type=int, default=5010)
    args = ap.parse_args()

    link = WifiLink(args.host, args.port)

    pygame.init()
    screen = pygame.display.set_mode((W, H))
    pygame.display.set_caption("MILA Control")
    clock = pygame.time.Clock()
    f_big = pygame.font.SysFont("monospace", 26, bold=True)
    f_med = pygame.font.SysFont("monospace", 15, bold=True)
    f_sml = pygame.font.SysFont("monospace", 12)

    mode = "wasd"   # mirrors the firmware's WASD boot default
    active_drive = None
    buttons = []

    def add(rect, label, cb, active=False):
        b = Button(rect, label, cb, f_med, active)
        buttons.append(b)
        return b

    def set_mode(m):
        nonlocal mode
        mode = m
        link.mode(m)
        for b, name in zip(mode_btns, ("wasd", "tank", "obstacle")):
            b.active = (name == m)

    mode_btns = [
        add((20, 100, 120, 40), "WASD", lambda: set_mode("wasd"), True),
        add((150, 100, 120, 40), "TANK", lambda: set_mode("tank")),
        add((280, 100, 120, 40), "OBSTACLE", lambda: set_mode("obstacle")),
    ]

    # Speed presets mirror the firmware's speedPresets[] (100/75/50/25).
    # speed_state tracks the last-known value, kept in sync from telemetry
    # each frame so it stays correct even if the IR remote or dashboard
    # changed it out from under us.
    speed_state = {"pct": 100}

    def adjust_speed(delta):
        new_pct = max(25, min(100, speed_state["pct"] + delta))
        speed_state["pct"] = new_pct
        link.cmd(f"SPEED:{new_pct}")

    speed_btns = [
        add((20, 150, 180, 40), "SPD -", lambda: adjust_speed(-25)),
        add((220, 150, 180, 40), "SPD +", lambda: adjust_speed(25)),
    ]

    # WASD d-pad
    pad_cx, pad_cy, bs, gap = W // 2, 280, 70, 8
    def start_drive(action):
        nonlocal active_drive
        active_drive = action
        link.cmd(action)

    def stop_drive():
        nonlocal active_drive
        if active_drive:
            active_drive = None
            link.cmd("STOP")

    wasd_btns = {
        "FORWARD": add((pad_cx - bs // 2, pad_cy - bs - gap, bs, bs), "^", lambda: start_drive("FORWARD")),
        "BACKWARD": add((pad_cx - bs // 2, pad_cy + gap, bs, bs), "v", lambda: start_drive("BACKWARD")),
        "LEFT": add((pad_cx - bs - gap - bs // 2 - gap, pad_cy - bs // 2, bs, bs), "<", lambda: start_drive("LEFT")),
        "RIGHT": add((pad_cx + gap + bs // 2 + gap, pad_cy - bs // 2, bs, bs), ">", lambda: start_drive("RIGHT")),
    }
    stop_btn = add((pad_cx - bs // 2, pad_cy - bs // 2, bs, bs), "STOP", lambda: link.cmd("STOP"))

    # TANK track buttons
    tank_y = 280
    tank_btns = [
        add((60, tank_y - 40, 90, 50), "Q\nL-FWD", lambda: start_drive("L_FWD")),
        add((60, tank_y + 40, 90, 50), "A\nL-BWD", lambda: start_drive("L_BWD")),
        add((W - 150, tank_y - 40, 90, 50), "E\nR-FWD", lambda: start_drive("R_FWD")),
        add((W - 150, tank_y + 40, 90, 50), "D\nR-BWD", lambda: start_drive("R_BWD")),
    ]

    key_drive_wasd = {
        pygame.K_UP: "FORWARD", pygame.K_w: "FORWARD",
        pygame.K_DOWN: "BACKWARD", pygame.K_s: "BACKWARD",
        pygame.K_LEFT: "LEFT", pygame.K_a: "LEFT",
        pygame.K_RIGHT: "RIGHT", pygame.K_d: "RIGHT",
    }
    key_drive_tank = {
        pygame.K_q: "L_FWD", pygame.K_a: "L_BWD",
        pygame.K_e: "R_FWD", pygame.K_d: "R_BWD",
    }

    running = True
    while running:
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                running = False

            elif ev.type == pygame.KEYDOWN:
                if ev.key == pygame.K_ESCAPE:
                    running = False
                elif ev.key == pygame.K_1:
                    set_mode("wasd")
                elif ev.key == pygame.K_2:
                    set_mode("tank")
                elif ev.key == pygame.K_3:
                    set_mode("obstacle")
                elif ev.key == pygame.K_SPACE:
                    stop_drive()
                elif mode == "wasd" and ev.key in key_drive_wasd:
                    start_drive(key_drive_wasd[ev.key])
                elif mode == "tank" and ev.key in key_drive_tank:
                    start_drive(key_drive_tank[ev.key])

            elif ev.type == pygame.KEYUP:
                keymap = key_drive_wasd if mode == "wasd" else key_drive_tank
                if ev.key in keymap and active_drive == keymap[ev.key]:
                    stop_drive()

            elif ev.type == pygame.MOUSEBUTTONDOWN:
                for b in buttons:
                    if b.hit(ev.pos):
                        b.pressed = True
                        b.cb()

            elif ev.type == pygame.MOUSEBUTTONUP:
                for b in buttons:
                    b.pressed = False
                if active_drive:
                    stop_drive()

        # ---- draw ----
        screen.fill(BG)
        t = link.telemetry()
        if "speed" in t:
            try:
                speed_state["pct"] = int(float(t["speed"]))
            except (TypeError, ValueError):
                pass

        title = f_big.render("MILA", True, ACCENT)
        screen.blit(title, title.get_rect(centerx=W // 2, y=14))
        conn = "connected" if link.ok else "OFFLINE"
        col = GREEN if link.ok else RED
        sub = f_sml.render(f"{args.host}:{args.port}   {conn}", True, col)
        screen.blit(sub, sub.get_rect(centerx=W // 2, y=46))

        for b in mode_btns:
            b.draw(screen)

        for b in speed_btns:
            b.draw(screen)

        if mode == "wasd":
            for b in wasd_btns.values():
                b.draw(screen)
            stop_btn.draw(screen)
        elif mode == "tank":
            for b in tank_btns:
                b.draw(screen)

        # status tiles
        sy = 400
        colw = (W - 50) // 2
        stat_tile(screen, f_sml, f_med, 20, sy, colw, "FRONT CM", t.get("dist", "--"))
        stat_tile(screen, f_sml, f_med, 30 + colw, sy, colw, "LEFT CM", t.get("left", "--"))
        stat_tile(screen, f_sml, f_med, 20, sy + 54, colw, "RIGHT CM", t.get("right", "--"))
        stat_tile(screen, f_sml, f_med, 30 + colw, sy + 54, colw, "LAST CMD", t.get("cmd", "--"))
        stat_tile(screen, f_sml, f_med, 20, sy + 108, colw, "LAST IR", t.get("ir", "--"))
        stat_tile(screen, f_sml, f_med, 30 + colw, sy + 108, colw, "TEMP C", t.get("temp", "--"))
        stat_tile(screen, f_sml, f_med, 20, sy + 162, colw, "HUMIDITY %", t.get("hum", "--"))
        fleet_txt = "FLEET (joined NORA)" if t.get("fleet") == 1 else "STANDALONE AP"
        stat_tile(screen, f_sml, f_med, 30 + colw, sy + 162, colw, "IP  " + str(t.get("ip", "--")), fleet_txt)
        guard_txt = "ACTIVE" if str(t.get("guard")) == "1" else "clear"
        stat_tile(screen, f_sml, f_med, 20, sy + 216, colw, "SPEED %", t.get("speed", "--"))
        stat_tile(screen, f_sml, f_med, 30 + colw, sy + 216, colw, "GUARD", guard_txt)

        hint = f_sml.render("1/2/3 mode  ·  arrows/WASD or QAED drive  ·  space stop  ·  esc quit", True, DIM)
        screen.blit(hint, hint.get_rect(centerx=W // 2, y=H - 24))

        pygame.display.flip()
        clock.tick(60)

    pygame.quit()


if __name__ == "__main__":
    main()
