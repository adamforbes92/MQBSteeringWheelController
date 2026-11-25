/*
  ForbesLin - a small blocking LIN master for the ESP32 (Arduino HardwareSerial).

  Written for the MFSW Controller (V1.20) to replace the vendored
  "LIN master portable" library, whose ESP32 port missed frames:
    - its BREAK was a 0x00 at half speed with the speed switched back exactly
      two byte-times later, without waiting for the byte to leave the UART;
    - its timeout was twice the nominal frame time counted from BEFORE the
      BREAK, so a wheel answering late (but within the LIN spec) "timed out";
    - it busy-waited on Serial.available() without ever yielding.

  Here:
    - BREAK = 0x00 at half the bus speed, then flush() (which blocks until the
      byte has really been shifted out: uart_wait_tx_done) before the speed
      goes back up - the same method the controller's Passthru relay uses.
    - The response timeout runs from the end of the BREAK, sized to the LIN
      spec's 1.4 x nominal for SYNC + PID + data + checksum, plus 2 ms for
      the UART's receive-idle delivery delay.
    - Waiting yields the CPU in 1 ms steps; the UART buffers in hardware, so
      nothing is lost while the task sleeps.

  It keeps the old library's names (LIN_Master_Base / _HardwareSerial_ESP32,
  error and version enums, getRawRxBuffer(), setBaudrate(), getTiming()) so
  the firmware above it is unchanged. The raw buffer has the same layout:
  [0] BREAK echo, [1] SYNC 0x55, [2] PID, then data, then checksum.
*/
#pragma once

#include <Arduino.h>

class ForbesLinMaster
{
public:
  /// LIN version: selects the checksum (V1 classic = data only, V2 enhanced = PID + data).
  typedef enum : uint8_t
  {
    LIN_V1 = 1,
    LIN_V2 = 2
  } version_t;

  /// Error flags (same values as the old library, so logs read the same).
  typedef enum : uint8_t
  {
    NO_ERROR      = 0x00,
    ERROR_STATE   = 0x01,  //!< used incorrectly (e.g. port not open)
    ERROR_ECHO    = 0x02,  //!< our own bits didn't read back (header or master data)
    ERROR_TIMEOUT = 0x04,  //!< the frame didn't complete in time
    ERROR_CHK     = 0x08,  //!< slave response checksum wrong
    ERROR_MISC    = 0x80
  } error_t;

  /// Per-transaction timing for the "LIN1 ... TIMING" diagnostic. All in
  /// microseconds after the BREAK started: body = header (SYNC+PID[+data])
  /// written, header = header echo received, done = finished / gave up,
  /// timeout = the limit applied, maxGap = longest gap between two checks of
  /// the receive loop (about 1 ms normally - it sleeps 1 ms per check).
  struct Timing
  {
    uint32_t startUs, bodyUs, headerUs, doneUs, timeoutUs, maxGapUs;
    uint8_t lenRx;
  };

  ForbesLinMaster(HardwareSerial& port, uint8_t pinRx, uint8_t pinTx, const char* name = "LIN");

  /// Open the port (8N1) at `baud` on the pins given to the constructor.
  void begin(uint16_t baud = 19200);

  /// Bus speed used from the next transaction on (the port itself is retuned
  /// at every transaction, so this is all a speed change needs).
  void setBaudrate(uint16_t baud);
  uint16_t getBaudrate() const { return baud_; }

  /// Send a master request: BREAK, SYNC, PID, data, checksum. Checks the echo.
  error_t sendMasterRequestBlocking(version_t version, uint8_t id, uint8_t numData, uint8_t data[]);

  /// Ask a slave for its response: BREAK, SYNC, PID, then read numData + checksum.
  /// `data` receives the data bytes (zeros where nothing arrived).
  error_t receiveSlaveResponseBlocking(version_t version, uint8_t id, uint8_t numData, uint8_t* data);

  error_t getError() const { return error_; }
  void resetError() { error_ = NO_ERROR; }
  void resetStateMachine() {}  // blocking only: there is no background state
  uint8_t handler() { return 0; }  // blocking only: nothing to service

  /// The whole receive buffer as captured: [0] BREAK echo, [1] SYNC, [2] PID,
  /// data..., checksum. `len` = bytes expected for the last frame.
  const uint8_t* getRawRxBuffer(uint8_t& len)
  {
    len = lenRx_;
    return bufRx_;
  }

  Timing getTiming() const { return timing_; }

  static uint8_t protectedId(uint8_t id);
  static uint8_t checksum(version_t version, uint8_t pid, const uint8_t* data, uint8_t numData);

private:
  error_t transact(version_t version, uint8_t id, const uint8_t* txData, uint8_t txLen, uint8_t rxData);

  HardwareSerial* port_;
  uint8_t pinRx_, pinTx_;
  const char* name_;
  uint16_t baud_ = 19200;
  error_t error_ = NO_ERROR;
  uint8_t bufRx_[12] = {};
  uint8_t lenRx_ = 0;
  Timing timing_ = {};
};

// The old library's names, so the firmware compiles unchanged.
using LIN_Master_Base = ForbesLinMaster;
using LIN_Master_HardwareSerial_ESP32 = ForbesLinMaster;
