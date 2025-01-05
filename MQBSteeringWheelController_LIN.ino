void getLightLINFrame() {
  memset(gatewayLightData, '\0', arraySize(gatewayLightData));  // clear old buttons

  chassisLIN.resetStateMachine();
  chassisLIN.resetError();
  chassisLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linLightID, 4, gatewayLightData);
}

void getButtonState() {
  memset(recvButtonData, '\0', arraySize(recvButtonData));  // clear old buttons
  memset(transButtonDataLIN, '\0', arraySize(transButtonDataLIN));  // clear old buttons
  memset(transButtonDataCAN, '\0', arraySize(transButtonDataCAN));  // clear old buttons

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linButtonID, 8, recvButtonData);
}

void sendLightLINFrame() {
  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linLightID, 4, steeringWheelLightData);
}

void sendButtonLINFrame() {
  buttonFound = false;

  if (recvButtonData[1] != 0) {  // button pressed; parse it...

#if stateDebug
    for (int j = 0; j < arraySize(recvButtonData); j++) {  // print ALL data from the received buttons - byte 0 through 7
      Serial.println(recvButtonData[j]);
    }
#endif

    for (int i = 1; i < arraySize(buttonTranspose); i++) {
      if (recvButtonData[1] == buttonTranspose[i].fromID) {  // start at 1, 0 is ignored...

#if stateDebug
        Serial.println(buttonTranspose[i].comment);
#endif

        transButtonDataLIN[1] = buttonTranspose[i].toID;   // found buttons, transfer the 'toID' into the LIN frame
        transButtonDataCAN[1] = buttonTranspose[i].canID;  // found buttons, transfer the 'toCAN' into the CAN frame

        chassisLIN.resetStateMachine();
        chassisLIN.resetError();
        chassisLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linButtonID, 8, transButtonDataLIN);

        buttonFound = true;
      }
    }
  }

  if (!buttonFound && recvButtonData[1] != 0) {
    Serial.println("Button not found, program it?");
  }
}
