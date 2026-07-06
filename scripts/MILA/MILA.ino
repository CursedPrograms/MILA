#include <Servo.h>
#include <IRremote.hpp>
#include <Wire.h>

// === PINS ===
const int trigPin = 2;
const int echoPin = 3;
const int irPin = A3;

// Motor R
const int IN1 = 4;
const int IN2 = 5;
const int ENA = 9;

// Motor L
const int IN3 = 6;
const int IN4 = 7;
const int ENB = 10;

// RGB LED (A0=R, A1=G, A2=B)
const int ledR = A0;
const int ledG = A1;
const int ledB = A2;

// Servo on pin 11
Servo myServo;

// === TUNING ===
int turnTime = 550; // ms — increase for wider turns
int stopDist = 35;  // cm — how close before stopping

// === SPEED ===
// Single-button speed control (X on the dashboard keyboard, OK on the IR
// remote) cycles through these presets rather than needing separate +/-,
// since the remote only has the one spare button to give it.
const int speedPresets[] = {100, 75, 50, 25};
const int NUM_SPEED_PRESETS = 4;
int speedIndex = 0;
int speedPct   = 100;

int pwmSpeed() { return (255 * speedPct) / 100; }

void cycleSpeed() {
  speedIndex = (speedIndex + 1) % NUM_SPEED_PRESETS;
  speedPct   = speedPresets[speedIndex];
  Serial.print("SPEED:"); Serial.println(speedPct);
}

// === IR REMOTE ===
// Codes read directly off this remote via the dashboard's "LAST IR" readout.
#define IR_FORWARD  0x74
#define IR_BACKWARD 0x75
#define IR_LEFT     0x34
#define IR_RIGHT    0x33
#define IR_OK       0x65

#define IR_MODE_WASD     0x0   // "1"
#define IR_MODE_TANK     0x1   // "2"
#define IR_MODE_OBSTACLE 0x2   // "3"

#define IR_RED    0x25   // tank: left track forward
#define IR_GREEN  0x26   // tank: left track backward
#define IR_YELLOW 0x27   // tank: right track forward
#define IR_BLUE   0x24   // tank: right track backward

const unsigned long IR_HOLD_MS = 350; // no repeat frame within this long = WASD button released
unsigned long lastIRMs  = 0;
uint8_t       lastIRCmd = 0;
bool          irDriving = false;   // true while a held WASD-mode IR key is driving the motors

// Tank mode is button-toggled, not held: an IR receiver only ever decodes
// one button at a time, so "hold both tracks forward" isn't possible the
// way a two-stick controller would do it. Each color latches its track on
// or off instead, so pressing RED then YELLOW drives both tracks forward.
bool leftFwdOn = false, leftBwdOn = false, rightFwdOn = false, rightBwdOn = false;

// === AHT10 (I2C temp/humidity) ===
#define AHT10_ADDR 0x38
const unsigned long ENV_INTERVAL_MS = 2000; // how often to poll the sensor
float lastTemp     = 0;
float lastHumidity = 0;

// === LIGHTING ===
unsigned long lastStrobeTime = 0;
bool strobeState = false;

// === SEQUENCES ===
const int cmySeq[3][3] = {
  {0,   255, 255},
  {255, 0,   255},
  {255, 255, 0  }
};

// === MODE & STATE ===
enum DriveMode { MODE_WASD, MODE_TANK, MODE_OBSTACLE };
DriveMode driveMode = MODE_WASD;   // WASD is the boot default

enum RobotState { MOVING_FORWARD, TURNING, STOPPED };
RobotState robotState = STOPPED;

// === SENSOR CACHE (for ESP reporting) ===
long lastDist  = 0;
long lastLeft  = 0;
long lastRight = 0;

// =====================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(ENA, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT); pinMode(ENB, OUTPUT);

  pinMode(ledR, OUTPUT);
  pinMode(ledG, OUTPUT);
  pinMode(ledB, OUTPUT);

  myServo.attach(11);

  IrReceiver.begin(irPin, ENABLE_LED_FEEDBACK);

  Wire.begin();
  delay(20);
  initAHT10();

  // Servo sweep test
  myServo.write(30);  delay(500);
  myServo.write(150); delay(500);
  myServo.write(90);  delay(300);

  stopMotors();

  // Startup CMY sequence
  for (int i = 0; i < 3; i++) {
    setColor(cmySeq[i][0], cmySeq[i][1], cmySeq[i][2]);
    delay(333);
  }
  setColor(0, 0, 0);
  delay(200);

  Serial.println("DIST:0");
  Serial.println("LEFT:0");
  Serial.println("RIGHT:0");
  Serial.println("TURN:");
  Serial.print("SPEED:"); Serial.println(speedPct);
}

// =====================
void loop() {
  checkIR();
  checkSerial();
  updateEnvSensor();

  if (driveMode != MODE_OBSTACLE) {
    updateLighting();
    return;
  }

  // === OBSTACLE AVOIDANCE ===
  myServo.write(90);
  delay(300);
  lastDist = getDistance();

  Serial.print("DIST:"); Serial.println(lastDist);

  if (lastDist > stopDist) {
    forward();
    robotState = MOVING_FORWARD;
    updateLighting();

  } else {
    stopMotors();
    robotState = STOPPED;
    updateLighting();
    delay(300);

    // Scan left
    myServo.write(150);
    delay(500);
    lastLeft = getDistance();
    Serial.print("LEFT:"); Serial.println(lastLeft);

    // Scan right
    myServo.write(30);
    delay(500);
    lastRight = getDistance();
    Serial.print("RIGHT:"); Serial.println(lastRight);

    // Return to center
    myServo.write(90);
    delay(300);

    robotState = TURNING;
    updateLighting();

    if (lastLeft > lastRight) {
      Serial.println("TURN:LEFT");
      turnLeft();
    } else {
      Serial.println("TURN:RIGHT");
      turnRight();
    }

    stopMotors();
    robotState = STOPPED;
    Serial.println("TURN:");
    delay(100);
  }

  updateLighting();
}

// =====================
void initAHT10() {
  Wire.beginTransmission(AHT10_ADDR);
  Wire.write(0x71);
  Wire.endTransmission();
  Wire.requestFrom(AHT10_ADDR, 1);
  uint8_t status = Wire.available() ? Wire.read() : 0;

  if ((status & 0x68) != 0x08) {   // calibration bit not set — (re)initialize
    Wire.beginTransmission(AHT10_ADDR);
    Wire.write(0xE1); Wire.write(0x08); Wire.write(0x00);
    Wire.endTransmission();
    delay(10);
  }
}

// =====================
void updateEnvSensor() {
  static unsigned long lastEnvMs = 0;
  if (millis() - lastEnvMs < ENV_INTERVAL_MS) return;
  lastEnvMs = millis();

  Wire.beginTransmission(AHT10_ADDR);
  Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return;   // sensor not responding

  delay(80);   // AHT10 needs ~75ms to complete a measurement

  Wire.requestFrom(AHT10_ADDR, 6);
  if (Wire.available() < 6) return;

  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();
  if (buf[0] & 0x80) return;   // busy flag still set — try again next cycle

  uint32_t rawHum  = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | (buf[3] >> 4);
  uint32_t rawTemp = ((uint32_t)(buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];

  lastHumidity = (rawHum  / 1048576.0) * 100.0;
  lastTemp     = (rawTemp / 1048576.0) * 200.0 - 50.0;

  Serial.print("TEMP:"); Serial.println(lastTemp, 1);
  Serial.print("HUM:");  Serial.println(lastHumidity, 1);
}

// =====================
void checkIR() {
  if (IrReceiver.decode()) {
    if (IrReceiver.decodedIRData.protocol != UNKNOWN) {
      bool isRepeat = IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT;
      if (!isRepeat) lastIRCmd = IrReceiver.decodedIRData.command;
      lastIRMs = millis();

      Serial.print("IR:0x");
      Serial.println(lastIRCmd, HEX);

      if (!isRepeat) runIRCommand(lastIRCmd);
    }
    IrReceiver.resume();

  } else if (driveMode == MODE_WASD && irDriving && millis() - lastIRMs > IR_HOLD_MS) {
    // no repeat frame arrived in time — treat the WASD button as released
    stopMotors();
    robotState = STOPPED;
    irDriving  = false;
    updateLighting();
  }
}

// =====================
void resetManualState() {
  stopMotors();
  robotState = STOPPED;
  irDriving  = false;
  leftFwdOn = leftBwdOn = rightFwdOn = rightBwdOn = false;
}

// Applies the four tank-track toggle latches to the motor pins.
void applyTankMotors() {
  if      (leftFwdOn)  leftMotorFwd();
  else if (leftBwdOn)  leftMotorBwd();
  else                 leftMotorOff();

  if      (rightFwdOn) rightMotorFwd();
  else if (rightBwdOn) rightMotorBwd();
  else                 rightMotorOff();

  robotState = (leftFwdOn || leftBwdOn || rightFwdOn || rightBwdOn) ? MOVING_FORWARD : STOPPED;
  updateLighting();
}

// =====================
void runIRCommand(uint8_t cmd) {
  switch (cmd) {
    case IR_MODE_WASD:
      driveMode = MODE_WASD;
      resetManualState();
      break;

    case IR_MODE_TANK:
      driveMode = MODE_TANK;
      resetManualState();
      break;

    case IR_MODE_OBSTACLE:
      driveMode = MODE_OBSTACLE;
      resetManualState();
      break;

    case IR_OK:
      cycleSpeed();
      break;

    case IR_FORWARD:
      if (driveMode == MODE_WASD) { forward();   robotState = MOVING_FORWARD; irDriving = true; }
      break;

    case IR_BACKWARD:
      if (driveMode == MODE_WASD) { backward();  robotState = MOVING_FORWARD; irDriving = true; }
      break;

    case IR_LEFT:
      if (driveMode == MODE_WASD) { turnLeft();  robotState = TURNING;        irDriving = true; }
      break;

    case IR_RIGHT:
      if (driveMode == MODE_WASD) { turnRight(); robotState = TURNING;        irDriving = true; }
      break;

    case IR_RED:
      if (driveMode == MODE_TANK) { leftFwdOn = !leftFwdOn; leftBwdOn = false; applyTankMotors(); }
      break;

    case IR_GREEN:
      if (driveMode == MODE_TANK) { leftBwdOn = !leftBwdOn; leftFwdOn = false; applyTankMotors(); }
      break;

    case IR_YELLOW:
      if (driveMode == MODE_TANK) { rightFwdOn = !rightFwdOn; rightBwdOn = false; applyTankMotors(); }
      break;

    case IR_BLUE:
      if (driveMode == MODE_TANK) { rightBwdOn = !rightBwdOn; rightFwdOn = false; applyTankMotors(); }
      break;
  }
  updateLighting();
}

// =====================
void checkSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd == "OBSTACLE") {
    driveMode = MODE_OBSTACLE;
    resetManualState();

  } else if (cmd == "TANK") {
    driveMode = MODE_TANK;
    resetManualState();

  } else if (cmd == "WASD" || cmd == "MANUAL") {
    driveMode = MODE_WASD;
    resetManualState();

  } else if (cmd == "SPEEDCYCLE") {
    cycleSpeed();

  } else if (driveMode == MODE_OBSTACLE) {
    return;

  } else if (cmd == "FORWARD") {
    forward();
    robotState = MOVING_FORWARD;

  } else if (cmd == "BACKWARD") {
    backward();
    robotState = MOVING_FORWARD;

  } else if (cmd == "LEFT") {
    turnLeft();
    robotState = TURNING;

  } else if (cmd == "RIGHT") {
    turnRight();
    robotState = TURNING;

  } else if (cmd == "STOP") {
    stopMotors();
    robotState = STOPPED;

  } else if (cmd == "L_FWD") {
    leftMotorFwd();
    robotState = MOVING_FORWARD;

  } else if (cmd == "L_BWD") {
    leftMotorBwd();
    robotState = MOVING_FORWARD;

  } else if (cmd == "R_FWD") {
    rightMotorFwd();
    robotState = MOVING_FORWARD;

  } else if (cmd == "R_BWD") {
    rightMotorBwd();
    robotState = MOVING_FORWARD;
  }

  updateLighting();
}

// =====================
void updateLighting() {
  unsigned long now = millis();

  if (robotState == MOVING_FORWARD) {
    setColor(0, 0, 255);

  } else if (robotState == TURNING) {
    if (now - lastStrobeTime >= 100) {
      strobeState = !strobeState;
      lastStrobeTime = now;
      if (strobeState) setColor(255, 255, 255);
      else             setColor(0,   0,   0  );
    }

  } else {
    setColor(255, 0, 0);
  }
}

// =====================
void setColor(int r, int g, int b) {
  analogWrite(ledR, r);
  analogWrite(ledG, g);
  analogWrite(ledB, b);
}

// =====================
long getDistance() {
  long total = 0;
  for (int i = 0; i < 3; i++) {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    long duration = pulseIn(echoPin, HIGH, 38000);
    total += (duration == 0) ? 999 : duration * 0.0343 / 2;
    delay(10);
  }
  return total / 3;
}

// =====================
void stopMotors() {
  digitalWrite(ENA, LOW); digitalWrite(ENB, LOW);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void forward() {
  analogWrite(ENA, pwmSpeed()); analogWrite(ENB, pwmSpeed());
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void backward() {
  analogWrite(ENA, pwmSpeed()); analogWrite(ENB, pwmSpeed());
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

void turnLeft() {
  analogWrite(ENA, pwmSpeed()); analogWrite(ENB, pwmSpeed());
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
  if (driveMode == MODE_OBSTACLE) delay(turnTime);
}

void turnRight() {
  analogWrite(ENA, pwmSpeed()); analogWrite(ENB, pwmSpeed());
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  if (driveMode == MODE_OBSTACLE) delay(turnTime);
}

void leftMotorFwd() {
  analogWrite(ENB, pwmSpeed());
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void leftMotorBwd() {
  analogWrite(ENB, pwmSpeed());
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
}

void leftMotorOff() {
  analogWrite(ENB, 0);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void rightMotorFwd() {
  analogWrite(ENA, pwmSpeed());
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
}

void rightMotorBwd() {
  analogWrite(ENA, pwmSpeed());
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
}

void rightMotorOff() {
  analogWrite(ENA, 0);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
}
