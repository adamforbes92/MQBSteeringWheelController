// ForbesLin - see ForbesLin.h for why this exists and how it differs.
#include "ForbesLin.h"

ForbesLinMaster::ForbesLinMaster(HardwareSerial& port, uint8_t pinRx, uint8_t pinTx, const char* name)
    : port_(&port), pinRx_(pinRx), pinTx_(pinTx), name_(name)
{
  // Don't touch the port here: global constructors run before the core is up.
}

void ForbesLinMaster::begin(uint16_t baud)
{
  baud_ = baud ? baud : 19200;
  port_->end();
  port_->begin(baud_, SERIAL_8N1, pinRx_, pinTx_);
}

void ForbesLinMaster::setBaudrate(uint16_t baud)
{
  if (baud) baud_ = baud;
}

uint8_t ForbesLinMaster::protectedId(uint8_t id)
{
  id &= 0x3F;
  const uint8_t p0 = (uint8_t)((id ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 0x01);
  const uint8_t p1 = (uint8_t)(~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5)) & 0x01);
  return (uint8_t)(id | (p0 << 6) | (p1 << 7));
}

uint8_t ForbesLinMaster::checksum(version_t version, uint8_t pid, const uint8_t* data, uint8_t numData)
{
  // LIN 2.x enhanced checksum includes the PID; LIN 1.x classic doesn't, and
  // the diagnostic frames 0x3C / 0x3D are always classic.
  const uint8_t id = pid & 0x3F;
  uint16_t sum = (version == LIN_V1 || id == 0x3C || id == 0x3D) ? 0 : pid;
  for (uint8_t i = 0; i < numData; i++)
  {
    sum += data[i];
    if (sum > 0xFF) sum -= 0xFF;  // add with carry
  }
  return (uint8_t)(0xFF - (uint8_t)sum);
}

ForbesLinMaster::error_t ForbesLinMaster::sendMasterRequestBlocking(version_t version, uint8_t id, uint8_t numData,
                                                                    uint8_t data[])
{
  if (numData > 8) numData = 8;
  return transact(version, id, data, numData, 0);
}

ForbesLinMaster::error_t ForbesLinMaster::receiveSlaveResponseBlocking(version_t version, uint8_t id, uint8_t numData,
                                                                       uint8_t* data)
{
  if (numData > 8) numData = 8;
  const error_t err = transact(version, id, nullptr, 0, numData);
  if (data)
  {
    for (uint8_t i = 0; i < numData; i++) data[i] = bufRx_[3 + i];
  }
  return err;
}

// One LIN frame. txLen > 0: master request with txLen data bytes (we send
// them and read our own echo). rxData > 0: slave response of rxData bytes.
ForbesLinMaster::error_t ForbesLinMaster::transact(version_t version, uint8_t id, const uint8_t* txData, uint8_t txLen,
                                                   uint8_t rxData)
{
  memset(bufRx_, 0, sizeof(bufRx_));
  timing_ = {};

  if (!(*port_))
  {
    error_ = (error_t)(error_ | ERROR_STATE);
    return error_;
  }

  const uint8_t pid = protectedId(id);
  const uint8_t numData = txLen ? txLen : rxData;
  // Expected on RX: BREAK echo + SYNC + PID + data + checksum.
  lenRx_ = (uint8_t)(3 + numData + 1);

  // Anything still going out finishes first; stale input is dropped.
  port_->flush();
  while (port_->available()) port_->read();

  // BREAK: 0x00 at half speed = 9 low bits = 18 bit-times at the bus speed
  // (LIN needs >= 13), then wait until it has really left the UART before
  // switching back.
  timing_.startUs = micros();
  port_->updateBaudRate(baud_ / 2);
  port_->write((uint8_t)0x00);
  port_->flush();
  port_->updateBaudRate(baud_);

  uint8_t tx[11];
  uint8_t n = 0;
  tx[n++] = 0x55;
  tx[n++] = pid;
  if (txLen)
  {
    for (uint8_t i = 0; i < txLen; i++) tx[n++] = txData[i];
    tx[n++] = checksum(version, pid, txData, txLen);
  }
  port_->write(tx, n);
  const uint32_t headerAt = micros();
  timing_.bodyUs = headerAt - timing_.startUs;

  // Timeout from here: SYNC + PID + data + checksum, 10 bits each, x1.4 (the
  // LIN spec's frame tolerance), + 2 ms for the UART's receive-idle delivery.
  const uint32_t bitUs = (1000000UL + baud_ / 2) / baud_;
  const uint32_t frameUs = (uint32_t)(2 + numData + 1) * 10U * bitUs;
  const uint32_t limitUs = frameUs * 14U / 10U + 2000U;
  timing_.timeoutUs = timing_.bodyUs + limitUs;

  uint32_t lastCheck = micros();
  bool complete = false;
  while (true)
  {
    const uint32_t now = micros();
    const uint32_t gap = now - lastCheck;
    if (gap > timing_.maxGapUs) timing_.maxGapUs = gap;
    lastCheck = now;

    const int avail = port_->available();
    if (timing_.headerUs == 0 && avail >= 3) timing_.headerUs = now - timing_.startUs;
    if (avail >= lenRx_)
    {
      complete = true;
      timing_.doneUs = now - timing_.startUs;
      break;
    }
    if (now - headerAt > limitUs)
    {
      timing_.doneUs = now - timing_.startUs;
      break;
    }
    vTaskDelay(1);  // UART buffers in hardware; let other tasks run meanwhile
  }
  timing_.lenRx = lenRx_;

  if (!complete)
  {
    // Leave whatever arrived in the FIFO: the caller's diagnostics read it
    // (RAW residual / TIMING). The next transaction drops it anyway.
    error_ = (error_t)(error_ | ERROR_TIMEOUT);
    return error_;
  }

  port_->readBytes(bufRx_, lenRx_);

  // Our own header (and, for a master request, our data + checksum) must read back.
  if (bufRx_[0] != 0x00 || bufRx_[1] != 0x55 || bufRx_[2] != pid)
  {
    error_ = (error_t)(error_ | ERROR_ECHO);
    return error_;
  }
  if (txLen)
  {
    for (uint8_t i = 0; i < (uint8_t)(txLen + 1); i++)
    {
      if (bufRx_[3 + i] != tx[2 + i])
      {
        error_ = (error_t)(error_ | ERROR_ECHO);
        return error_;
      }
    }
    return error_;
  }

  // Slave response: verify its checksum.
  if (bufRx_[3 + numData] != checksum(version, pid, bufRx_ + 3, numData))
  {
    error_ = (error_t)(error_ | ERROR_CHK);
  }
  return error_;
}
