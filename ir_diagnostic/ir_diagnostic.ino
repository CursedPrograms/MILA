#include <Arduino.h>
#include <IRremote.hpp>

// Explicitly set to Digital Pin 6
const uint8_t IR_RECEIVE_PIN = 6; 

void setup() {
    Serial.begin(115200);
    while (!Serial) { ; } // Wait for serial connection

    // Initialize IR receiver on Pin 6
    IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

    Serial.println(F("========================================"));
    Serial.print(F("IR Reader Ready on Pin "));
    Serial.println(IR_RECEIVE_PIN);
    Serial.println(F("Press remote buttons to read HEX codes..."));
    Serial.println(F("========================================"));
}

void loop() {
    if (IrReceiver.decode()) {
        
        // Print protocol name, address, and command details
        IrReceiver.printIRResultShort(&Serial);

        // Print command HEX code if parsed
        if (IrReceiver.decodedIRData.command != 0) {
            Serial.print(F(" -> Command HEX: 0x"));
            Serial.println(IrReceiver.decodedIRData.command, HEX);
        } 
        // Fallback for raw / unknown pulse signals (common on generic DVD remotes)
        else if (IrReceiver.decodedIRData.decodedRawData != 0) {
            Serial.print(F(" -> Raw Data HEX: 0x"));
            Serial.println(IrReceiver.decodedIRData.decodedRawData, HEX);
        }

        IrReceiver.resume(); // Ready for next button press
    }
}