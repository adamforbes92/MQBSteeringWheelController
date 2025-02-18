void basicInit() {
#ifdef ENABLE_DEBUG
  Serial.begin(115200);
#endif
Serial.println("hi");
  DEBUG("VW Steering Wheel LIN Controller Initialising...");

  DEBUG("Preferences Initialising...");
  preferences.begin("settings", false);
  DEBUG("Preferences Initialised!");

  DEBUG("IO Pins Initialising...");
  setupPins();  // setup pins for IO
  DEBUG("IO Pins Initialised!");

  DEBUG("LIN Initialising...");
  steeringWheelLIN.begin(linBaud);
  chassisLIN.begin(linBaud);
  DEBUG("LIN Initialised!");

  DEBUG("CAN Initialising...");
  canInit();
  DEBUG("CAN Initialised!");

  DEBUG("VW Steering Wheel LIN Controller Initialised!");
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

  radioResistor.setup(resistorInc, resistorUD, resistorCS);
}