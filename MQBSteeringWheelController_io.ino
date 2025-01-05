void basicInit() {
#if stateDebug
  Serial.begin(115200);
  Serial.println(F("VW Steering Wheel LIN Controller Initialising..."));
#endif

#if stateDebug
  Serial.println(F("Preferences Initialising..."));
#endif
  preferences.begin("settings", false);

  setupPins();  // setup pins for IO
  steeringWheelLIN.begin(linBaud);

#if stateDebug
  Serial.println(F("VW Steering Wheel LIN Controller Initialised, LIN started!"));
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