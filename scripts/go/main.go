// MILA WiFi controller (Ebitengine) — Go port of mila_controller.py.
// Mirrors the onboard web dashboard.
//
// USAGE
//   mila_controller.exe
//   mila_controller.exe --host 192.168.4.1 --port 5010
//
//   If MILA joined NORA's network (fleet mode) instead of hosting her own
//   AP, she won't be at 192.168.4.1 anymore — check NORA's dashboard/fleet
//   registry (http://192.168.4.1:5000/robots) for MILA's actual IP and pass
//   it with --host.
//
// BUILD (first time, fetch dependencies)
//   go mod tidy
//   go build -ldflags "-H=windowsgui" -o mila_controller.exe .
//
// CONTROLS
//   1 / 2 / 3     WASD / TANK / OBSTACLE mode
//   WASD mode:    Arrow keys or WASD to drive, Space to stop
//   TANK mode:    Q/A = left track fwd/back, E/D = right track fwd/back
//   Esc           quit
//   Everything is also clickable with the mouse.
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"image/color"
	"log"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/inpututil"
	"github.com/hajimehoshi/ebiten/v2/text/v2"
	"github.com/hajimehoshi/ebiten/v2/vector"
	"golang.org/x/image/font/gofont/gomono"
	"golang.org/x/image/font/gofont/gomonobold"
)

// ----------------------------------------------------------------------------
// Transport
// ----------------------------------------------------------------------------

type WifiLink struct {
	base   string
	client *http.Client
	cmds   chan string

	mu   sync.Mutex
	ok   bool
	data map[string]string
}

func NewWifiLink(host string, port int) *WifiLink {
	l := &WifiLink{
		base:   fmt.Sprintf("http://%s:%d", host, port),
		client: &http.Client{Timeout: time.Second},
		cmds:   make(chan string, 64),
		data:   map[string]string{},
	}
	go l.poll()
	go l.work()
	return l
}

func (l *WifiLink) get(path string) ([]byte, error) {
	resp, err := l.client.Get(l.base + path)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	var buf bytes.Buffer
	_, err = buf.ReadFrom(resp.Body)
	return buf.Bytes(), err
}

func stringify(v interface{}) string {
	switch x := v.(type) {
	case string:
		return x
	case float64:
		return strconv.FormatFloat(x, 'f', -1, 64)
	case bool:
		if x {
			return "1"
		}
		return "0"
	default:
		return fmt.Sprint(x)
	}
}

func (l *WifiLink) poll() {
	for {
		body, err := l.get("/status")
		var raw map[string]interface{}
		if err == nil {
			err = json.Unmarshal(body, &raw)
		}
		l.mu.Lock()
		if err == nil {
			l.data = make(map[string]string, len(raw))
			for k, v := range raw {
				l.data[k] = stringify(v)
			}
			l.ok = true
		} else {
			l.ok = false
		}
		l.mu.Unlock()
		time.Sleep(400 * time.Millisecond)
	}
}

// One worker keeps commands in order (a STOP must never overtake the FORWARD
// that preceded it).
func (l *WifiLink) work() {
	for path := range l.cmds {
		if _, err := l.get(path); err != nil {
			// Offline: drop the backlog rather than replaying stale drive
			// commands when the link comes back.
			for drained := false; !drained; {
				select {
				case <-l.cmds:
				default:
					drained = true
				}
			}
		}
	}
}

func (l *WifiLink) send(path string) {
	select {
	case l.cmds <- path:
	default: // queue full (link down for a while): drop
	}
}

func (l *WifiLink) Cmd(action string) { l.send("/cmd?v=" + action) }
func (l *WifiLink) Mode(m string)     { l.send("/mode?v=" + m) }

// StopNow is a synchronous STOP for shutdown.
func (l *WifiLink) StopNow() { _, _ = l.get("/cmd?v=STOP") }

func (l *WifiLink) Online() bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.ok
}

func (l *WifiLink) Telemetry() map[string]string {
	l.mu.Lock()
	defer l.mu.Unlock()
	out := make(map[string]string, len(l.data))
	for k, v := range l.data {
		out[k] = v
	}
	return out
}

// ----------------------------------------------------------------------------
// UI — same dark/cyan palette as the onboard dashboard
// ----------------------------------------------------------------------------

var (
	bgC       = color.RGBA{10, 10, 15, 255}
	panelC    = color.RGBA{18, 18, 26, 255}
	borderC   = color.RGBA{30, 30, 46, 255}
	accentC   = color.RGBA{0, 212, 255, 255}
	redC      = color.RGBA{255, 68, 68, 255}
	greenC    = color.RGBA{0, 255, 136, 255}
	textC     = color.RGBA{224, 224, 240, 255}
	dimC      = color.RGBA{102, 102, 136, 255}
	accentBgC = color.RGBA{14, 34, 46, 255}
)

const (
	W = 420
	H = 720

	groupAlways = 0
	groupWASD   = 1
	groupTank   = 2
)

type Button struct {
	x, y, w, h float32
	label      string
	group      int
	cb         func()
	active     bool
	pressed    bool
}

func (b *Button) hit(px, py int) bool {
	fx, fy := float32(px), float32(py)
	return fx >= b.x && fx <= b.x+b.w && fy >= b.y && fy <= b.y+b.h
}

type Game struct {
	link *WifiLink
	host string
	port int

	mode        string // mirrors the firmware's WASD boot default
	activeDrive string
	speedPct    int
	buttons     []*Button
	modeBtns    [3]*Button
	quit        bool

	fBig, fMed, fSml *text.GoTextFace
	prevFocused      bool
}

func (g *Game) add(x, y, w, h float32, label string, group int, cb func()) *Button {
	b := &Button{x: x, y: y, w: w, h: h, label: label, group: group, cb: cb}
	g.buttons = append(g.buttons, b)
	return b
}

func (g *Game) startDrive(action string) {
	g.activeDrive = action
	g.link.Cmd(action)
}

func (g *Game) stopDrive() {
	if g.activeDrive != "" {
		g.activeDrive = ""
		g.link.Cmd("STOP")
	}
}

func (g *Game) setMode(m string) {
	g.mode = m
	g.link.Mode(m)
	for i, name := range [3]string{"wasd", "tank", "obstacle"} {
		g.modeBtns[i].active = name == m
	}
}

// Speed presets mirror the firmware's speedPresets[] (100/75/50/25).
func (g *Game) adjustSpeed(delta int) {
	g.speedPct += delta
	if g.speedPct < 25 {
		g.speedPct = 25
	}
	if g.speedPct > 100 {
		g.speedPct = 100
	}
	g.link.Cmd(fmt.Sprintf("SPEED:%d", g.speedPct))
}

func (g *Game) build() {
	g.modeBtns[0] = g.add(20, 100, 120, 40, "WASD", groupAlways, func() { g.setMode("wasd") })
	g.modeBtns[1] = g.add(150, 100, 120, 40, "TANK", groupAlways, func() { g.setMode("tank") })
	g.modeBtns[2] = g.add(280, 100, 120, 40, "OBSTACLE", groupAlways, func() { g.setMode("obstacle") })
	g.modeBtns[0].active = true

	g.add(20, 150, 180, 40, "SPD -", groupAlways, func() { g.adjustSpeed(-25) })
	g.add(220, 150, 180, 40, "SPD +", groupAlways, func() { g.adjustSpeed(25) })

	// WASD d-pad
	const cx, cy, bs, gap = W / 2, 280, 70, 8
	g.add(cx-bs/2, cy-bs-gap, bs, bs, "^", groupWASD, func() { g.startDrive("FORWARD") })
	g.add(cx-bs/2, cy+gap, bs, bs, "v", groupWASD, func() { g.startDrive("BACKWARD") })
	g.add(cx-bs-gap-bs/2-gap, cy-bs/2, bs, bs, "<", groupWASD, func() { g.startDrive("LEFT") })
	g.add(cx+gap+bs/2+gap, cy-bs/2, bs, bs, ">", groupWASD, func() { g.startDrive("RIGHT") })
	g.add(cx-bs/2, cy-bs/2, bs, bs, "STOP", groupWASD, func() { g.link.Cmd("STOP") })

	// TANK track buttons
	const ty = 280
	g.add(60, ty-40, 90, 50, "Q\nL-FWD", groupTank, func() { g.startDrive("L_FWD") })
	g.add(60, ty+40, 90, 50, "A\nL-BWD", groupTank, func() { g.startDrive("L_BWD") })
	g.add(W-150, ty-40, 90, 50, "E\nR-FWD", groupTank, func() { g.startDrive("R_FWD") })
	g.add(W-150, ty+40, 90, 50, "D\nR-BWD", groupTank, func() { g.startDrive("R_BWD") })
}

func (g *Game) visible(b *Button) bool {
	return b.group == groupAlways ||
		(b.group == groupWASD && g.mode == "wasd") ||
		(b.group == groupTank && g.mode == "tank")
}

var keyWASD = map[ebiten.Key]string{
	ebiten.KeyArrowUp: "FORWARD", ebiten.KeyW: "FORWARD",
	ebiten.KeyArrowDown: "BACKWARD", ebiten.KeyS: "BACKWARD",
	ebiten.KeyArrowLeft: "LEFT", ebiten.KeyA: "LEFT",
	ebiten.KeyArrowRight: "RIGHT", ebiten.KeyD: "RIGHT",
}

var keyTank = map[ebiten.Key]string{
	ebiten.KeyQ: "L_FWD", ebiten.KeyA: "L_BWD",
	ebiten.KeyE: "R_FWD", ebiten.KeyD: "R_BWD",
}

func (g *Game) keymap() map[ebiten.Key]string {
	switch g.mode {
	case "wasd":
		return keyWASD
	case "tank":
		return keyTank
	}
	return nil
}

func (g *Game) Update() error {
	if g.quit {
		g.link.StopNow()
		return ebiten.Termination
	}

	for _, k := range inpututil.AppendJustPressedKeys(nil) {
		switch k {
		case ebiten.KeyEscape:
			g.quit = true
		case ebiten.Key1:
			g.setMode("wasd")
		case ebiten.Key2:
			g.setMode("tank")
		case ebiten.Key3:
			g.setMode("obstacle")
		case ebiten.KeySpace:
			g.stopDrive()
		default:
			if a, ok := g.keymap()[k]; ok {
				g.startDrive(a)
			}
		}
	}
	for _, k := range inpututil.AppendJustReleasedKeys(nil) {
		if a, ok := g.keymap()[k]; ok && a == g.activeDrive {
			g.stopDrive()
		}
	}

	if inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonLeft) {
		mx, my := ebiten.CursorPosition()
		for _, b := range g.buttons {
			if g.visible(b) && b.hit(mx, my) {
				b.pressed = true
				b.cb()
				break
			}
		}
	}
	if inpututil.IsMouseButtonJustReleased(ebiten.MouseButtonLeft) {
		g.releaseAll()
	}

	// Don't leave the robot driving if the window loses focus mid-press.
	focused := ebiten.IsFocused()
	if g.prevFocused && !focused {
		g.releaseAll()
	}
	g.prevFocused = focused

	// Keep speed in sync with telemetry so it stays correct even if the IR
	// remote or dashboard changed it out from under us.
	if sp, ok := g.link.Telemetry()["speed"]; ok {
		if f, err := strconv.ParseFloat(sp, 64); err == nil {
			g.speedPct = int(f)
		}
	}
	return nil
}

func (g *Game) releaseAll() {
	for _, b := range g.buttons {
		b.pressed = false
	}
	g.stopDrive()
}

// ---- drawing ----

func drawText(dst *ebiten.Image, face *text.GoTextFace, s string, x, y float64, c color.Color, centerX, centerY bool) {
	lines := strings.Split(s, "\n")
	lineH := face.Size * 1.25
	totalH := lineH * float64(len(lines))
	top := y
	if centerY {
		top = y - totalH/2
	}
	for i, ln := range lines {
		w, _ := text.Measure(ln, face, 0)
		lx := x
		if centerX {
			lx = x - w/2
		}
		op := &text.DrawOptions{}
		op.GeoM.Translate(lx, top+float64(i)*lineH)
		op.ColorScale.ScaleWithColor(c)
		text.Draw(dst, ln, face, op)
	}
}

func get(t map[string]string, k string) string {
	if v, ok := t[k]; ok {
		return v
	}
	return "--"
}

func (g *Game) tile(dst *ebiten.Image, x, y, w float32, label, value string) {
	vector.DrawFilledRect(dst, x, y, w, 46, panelC, false)
	vector.StrokeRect(dst, x, y, w, 46, 1, borderC, false)
	drawText(dst, g.fSml, label, float64(x)+8, float64(y)+6, dimC, false, false)
	drawText(dst, g.fMed, value, float64(x)+8, float64(y)+22, accentC, false, false)
}

func (g *Game) Draw(dst *ebiten.Image) {
	dst.Fill(bgC)
	t := g.link.Telemetry()

	drawText(dst, g.fBig, "MILA", W/2, 28, accentC, true, true)
	ok := g.link.Online()
	status, sc := "OFFLINE", color.Color(redC)
	if ok {
		status, sc = "connected", greenC
	}
	drawText(dst, g.fSml, fmt.Sprintf("%s:%d   %s", g.host, g.port, status), W/2, 54, sc, true, true)

	for _, b := range g.buttons {
		if !g.visible(b) {
			continue
		}
		on := b.active || b.pressed
		fill, edge, fg := panelC, borderC, textC
		if on {
			fill, edge, fg = accentBgC, accentC, accentC
		}
		vector.DrawFilledRect(dst, b.x, b.y, b.w, b.h, fill, false)
		vector.StrokeRect(dst, b.x, b.y, b.w, b.h, 2, edge, false)
		drawText(dst, g.fMed, b.label, float64(b.x+b.w/2), float64(b.y+b.h/2), fg, true, true)
	}

	const sy, colw = 400, (W - 50) / 2
	const c2 = 30 + colw
	g.tile(dst, 20, sy, colw, "FRONT CM", get(t, "dist"))
	g.tile(dst, c2, sy, colw, "LEFT CM", get(t, "left"))
	g.tile(dst, 20, sy+54, colw, "RIGHT CM", get(t, "right"))
	g.tile(dst, c2, sy+54, colw, "LAST CMD", get(t, "cmd"))
	g.tile(dst, 20, sy+108, colw, "LAST IR", get(t, "ir"))
	g.tile(dst, c2, sy+108, colw, "TEMP C", get(t, "temp"))
	g.tile(dst, 20, sy+162, colw, "HUMIDITY %", get(t, "hum"))
	fleet := "STANDALONE AP"
	if get(t, "fleet") == "1" {
		fleet = "FLEET (joined NORA)"
	}
	g.tile(dst, c2, sy+162, colw, "IP  "+get(t, "ip"), fleet)
	g.tile(dst, 20, sy+216, colw, "SPEED %", get(t, "speed"))
	guard := "clear"
	if get(t, "guard") == "1" {
		guard = "ACTIVE"
	}
	g.tile(dst, c2, sy+216, colw, "GUARD", guard)

	drawText(dst, g.fSml, "1/2/3 mode - WASD/QAED drive - space stop - esc quit",
		W/2, H-18, dimC, true, true)
}

func (g *Game) Layout(_, _ int) (int, int) { return W, H }

func main() {
	host := flag.String("host", "192.168.4.1", "MILA's IP address")
	port := flag.Int("port", 5010, "MILA's web server port")
	flag.Parse()

	mono, err := text.NewGoTextFaceSource(bytes.NewReader(gomono.TTF))
	if err != nil {
		log.Fatal(err)
	}
	monoBold, err := text.NewGoTextFaceSource(bytes.NewReader(gomonobold.TTF))
	if err != nil {
		log.Fatal(err)
	}

	g := &Game{
		link:     NewWifiLink(*host, *port),
		host:     *host,
		port:     *port,
		mode:     "wasd",
		speedPct: 100,
		fBig:     &text.GoTextFace{Source: monoBold, Size: 26},
		fMed:     &text.GoTextFace{Source: monoBold, Size: 15},
		fSml:     &text.GoTextFace{Source: mono, Size: 12},
	}
	g.build()

	ebiten.SetWindowSize(W, H)
	ebiten.SetWindowTitle("MILA Control")
	if err := ebiten.RunGame(g); err != nil {
		log.Fatal(err)
	}
}
