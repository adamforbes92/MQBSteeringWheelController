void getLightLINFrame() {
  // get LIN light frame (from another gateway, only used in >MK4)
  memset(gatewayLightData, '\0', arraySize(gatewayLightData));  // clear old buttons

  chassisLIN.resetStateMachine();
  chassisLIN.resetError();
  chassisLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linLightID, 4, gatewayLightData);
}

void getButtonState() {
  // get LIN buttons from steering wheel
  memset(recvButtonData, '\0', arraySize(recvButtonData));          // clear old buttons
  memset(transButtonDataLIN, '\0', arraySize(transButtonDataLIN));  // clear old buttons
  memset(transButtonDataCAN, '\0', arraySize(transButtonDataCAN));  // clear old buttons

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linButtonID, 8, recvButtonData);
}

void sendLightLINFrame() {
  // send light data - could be zero (off) but means we still get button frames
  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linLightID, 4, steeringWheelLightData);

  chassisLIN.resetStateMachine();
  chassisLIN.resetError();
  chassisLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linLightID, 4, steeringWheelLightData);
}

void sendButtonLINFrame() {
  buttonFound = false;
  /* print lin traffic
  for (int k = 0; k < arraySize(recvButtonData); k++) {  // print ALL data from the received buttons - byte 0 through 7
    ////Serial.print(recvButtonData[k]);
  }
  Serial.println("");
  */

  switch (recvButtonData[6]) {
    case 1:
      dsgPaddleDown = true;
      break;

    case 2:
      dsgPaddleUp = true;
      break;

    default:
      break;
  }

  if (recvButtonData[1] != 0) {                            // button pressed; parse it...
    for (int j = 0; j < arraySize(recvButtonData); j++) {  // print ALL data from the received buttons - byte 0 through 7
      //Serial.println(recvButtonData[j]);
    }

    Serial.println(recvButtonData[1]);

    for (int i = 1; i < arraySize(buttonTranspose); i++) {
      if (recvButtonData[1] == buttonTranspose[i].fromID) {  // start at 1, 0 is ignored...
        Serial.println(buttonTranspose[i].comment);

        transButtonDataLIN[1] = buttonTranspose[i].toID;   // found buttons, transfer the 'toID' into the LIN frame
        transButtonDataCAN[1] = buttonTranspose[i].canID;  // found buttons, transfer the 'toCAN' into the CAN frame

        radioResistance = buttonTranspose[i].radioOhm;

        chassisLIN.resetStateMachine();
        chassisLIN.resetError();
        chassisLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linButtonID, 8, transButtonDataLIN);

        buttonFound = true;
      }
    }
  }

  if (!buttonFound && recvButtonData[1] != 0) {
    DEBUG("Button not found, program it?");
  }
}
