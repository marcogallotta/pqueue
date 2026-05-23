#ifdef ARDUINO

#include <Arduino.h>
#include "pqueue_doctor_session.h"

void setup() {
    Serial.begin(115200);
    delay(500);
    pqueue::doctor::runSession();
    delay(100);
    ESP.restart();
}

void loop() {}

#endif // ARDUINO
