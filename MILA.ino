#include <Servo.h>

// === PINS ===
const int trigPin = 2;
const int echoPin = 3;

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
bool manualMode = false;

enum RobotState { MOVING_FORWARD, TURNING, STOPPED };
RobotState robotState = STOPPED;

// === SENSOR CACHE (for ESP reporting) ===
long lastDist  = 0;
long lastLeft  = 0;
long lastRight = 0;

// =====================
void setup() {
  Serial.begin(115200);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(ENA, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT); pinMode(ENB, OUTPUT);

  pinMode(ledR, OUTPUT);
  pinMode(ledG, OUTPUT);
  pinMode(ledB, OUTPUT);

  myServo.attach(11);

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

  // Tell ESP we booted into obstacle mode
  Serial.println("DIST:0");
  Serial.println("LEFT:0");
  Serial.println("RIGHT:0");
  Serial.println("TURN:");
}

// =====================
void loop() {
  checkSerial();

  if (manualMode) {
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

    // Turn toward side with MORE space
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
void checkSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd == "OBSTACLE") {
    manualMode = false;
    stopMotors();
    robotState = STOPPED;

  } else if (cmd == "MANUAL") {
    manualMode = true;
    stopMotors();
    robotState = STOPPED;

  } else if (!manualMode) {
    return; // ignore drive commands in obstacle mode

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
    setColor(0, 0, 255);                  // Solid blue

  } else if (robotState == TURNING) {
    if (now - lastStrobeTime >= 100) {    // White strobe ~10Hz
      strobeState = !strobeState;
      lastStrobeTime = now;
      if (strobeState) setColor(255, 255, 255);
      else             setColor(0,   0,   0  );
    }

  } else {
    setColor(255, 0, 0);                  // Solid red
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
  digitalWrite(ENA, HIGH); digitalWrite(ENB, HIGH);
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void backward() {
  digitalWrite(ENA, HIGH); digitalWrite(ENB, HIGH);
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

void turnLeft() {
  digitalWrite(ENA, HIGH); digitalWrite(ENB, HIGH);
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
  if (!manualMode) delay(turnTime);
}

void turnRight() {
  digitalWrite(ENA, HIGH); digitalWrite(ENB, HIGH);
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  if (!manualMode) delay(turnTime);
}

// Individual motor control for dual mode
void leftMotorFwd() {
  digitalWrite(ENB, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void leftMotorBwd() {
  digitalWrite(ENB, HIGH);
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
}

void rightMotorFwd() {
  digitalWrite(ENA, HIGH);
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
}

void rightMotorBwd() {
  digitalWrite(ENA, HIGH);
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
}
