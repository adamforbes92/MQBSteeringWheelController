void canInit() {
  chassisCAN.setRX(pinCAN_RX);
  chassisCAN.setTX(pinCAN_TX);
  chassisCAN.begin();
  chassisCAN.setBaudRate(500000);
  chassisCAN.onReceive(onBodyRX);

  // set filters up for focusing on only MOT1 / MOT 2?
}

void onBodyRX(const CAN_message_t& frame) {
#if ChassisCANDebug  // print incoming CAN messages
  // print CAN messages to Serial
  Serial.print("Length Recv: ");
  Serial.print(frame.len);
  Serial.print(" CAN ID: ");
  Serial.print(frame.id, HEX);
  Serial.print(" Buffer: ");
  for (uint8_t i = 0; i < frame.len; i++) {
    Serial.print(frame.buf[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
#endif
}

void broadcastButtonsCAN() {
  CAN_message_t broadcastCAN;
  broadcastCAN.id = canButtonID;
  broadcastCAN.len = 8;

  if (transButtonDataCAN[1] != 0) {  // button pressed; parse it...
    broadcastCAN.buf[1] = transButtonDataCAN[1];
  }

  if (!chassisCAN.write(broadcastCAN)) {              // write CAN frame from the body to the Haldex
    Serial.println(F("Chassis CAN Write TX Fail!"));  // if writing is unsuccessful, there is something wrong with the Haldex(!) Possibly flash red LED?
  }
}

void sendPaddleUpFrame() {
  CAN_message_t paddlesUp;  //0x7C0
  paddlesUp.id = GRA_ID;
  paddlesUp.len = 8;
  paddlesUp.buf[0] = 0xB7;  //
  paddlesUp.buf[2] = 0x34;
  paddlesUp.buf[3] = 0x02;
  bitSet(paddlesUp.buf[3], 1);         // set high (trigger)
  if (!chassisCAN.write(paddlesUp)) {  // write CAN frame from the body to the Haldex
  }

  paddlesUp.id = GRA_ID;
  paddlesUp.len = 8;
  paddlesUp.buf[0] = 0xB7;  //
  paddlesUp.buf[2] = 0x34;
  paddlesUp.buf[3] = 0x02;
  bitSet(paddlesUp.buf[3], 0);         // set low (off)
  if (!chassisCAN.write(paddlesUp)) {  // write CAN frame from the body to the Haldex
  }
  dsgPaddleUp = false;
  Serial.println("sent shift up message");
}

void sendPaddleDownFrame() {
  CAN_message_t paddlesDown;  //0x7C0
  paddlesDown.id = GRA_ID;
  paddlesDown.len = 8;
  paddlesDown.buf[0] = 0xB4;  //
  paddlesDown.buf[2] = 0x34;
  paddlesDown.buf[3] = 0x01;
  bitSet(paddlesDown.buf[3], 1);         // set high (trigger)
  if (!chassisCAN.write(paddlesDown)) {  // write CAN frame from the body to the Haldex
  }

  paddlesDown.id = GRA_ID;
  paddlesDown.len = 8;
  paddlesDown.buf[0] = 0xB4;  //
  paddlesDown.buf[2] = 0x34;
  paddlesDown.buf[3] = 0x01;
  bitSet(paddlesDown.buf[3], 0);         // set low (off)
  if (!chassisCAN.write(paddlesDown)) {  // write CAN frame from the body to the Haldex
  }
  Serial.println("sent shift down message");
  dsgPaddleDown = false;
}