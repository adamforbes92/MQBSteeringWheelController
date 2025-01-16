void basicInit() {
#if stateDebug
  Serial.begin(115200);
  Serial.println(F("VW Steering Wheel LIN Controller Initialising..."));
#endif

#if stateDebug
  Serial.println(F("Preferences Initialising..."));
#endif
  preferences.begin("settings", false);
#if stateDebug
  Serial.println(F("Preferences Initialised!"));
#endif

#if stateDebug
  Serial.println(F("IO Pins Initialising..."));
#endif
  setupPins();  // setup pins for IO
#if stateDebug
  Serial.println(F("IO Pins Initialised!"));
#endif

#if stateDebug
  Serial.println(F("LIN Initialising..."));
#endif
  steeringWheelLIN.begin(linBaud);
  chassisLIN.begin(linBaud);
#if stateDebug
  Serial.println(F("LIN Initialised!"));
#endif

#if stateDebug
  Serial.println(F("CAN Initialising..."));
#endif
canInit();
#if stateDebug
  Serial.println(F("CAN Initialised!"));
#endif

#if stateDebug
  Serial.println(F("VW Steering Wheel LIN Controller Initialised!"));
#endif
}

void setupPins() {
  // setup the pins for input/output
  pinMode(pinCS_LIN, OUTPUT);    // chip select pin for the TJA1020
  pinMode(pinWake_LIN, OUTPUT);  // wake pin for the TJA1020
  pinMode(pinAuxLight, INPUT);
  attachInterrupt(pinAuxLight, auxLightPWM, CHANGE);

  // drive CS & Wake high to use the LIN chip
  digitalWrite(pinCS_LIN, HIGH);
  digitalWrite(pinWake_LIN, HIGH);
}