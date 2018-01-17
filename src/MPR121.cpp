// ----------------------------------------------------------------------------
// MPR121.cpp
//
// Authors:
// Jim Lindblom
// Stefan Dzisiewski-Smith
// Peter Krige
// Peter Polidoro polidorop@janelia.hhmi.org
// ----------------------------------------------------------------------------
#include "MPR121.h"

MPR121::MPR121()
{
  wire_ptr_ = &Wire;
  address_ = 0x5A;
  ecr_backup_ = 0x00;
  error_byte_ = 1<<NOT_INITIALIZED_BIT; // initially, we're not initialised
  running_ = false;
  touch_data_ = 0;
  touch_data_previous_ = 0;
  auto_touch_status_flag_ = false;
}

void MPR121::setWire(TwoWire & wire)
{
  wire_ptr_ = &wire;
}

bool MPR121::begin()
{
  wire_ptr_->begin();

  error_byte_ &= ~(1<<NOT_INITIALIZED_BIT); // clear NOT_INITIALIZED error flag

  if(reset())
  {
    applySettings(default_settings_);
    return SUCCESS;
  }
  return !SUCCESS;
}

bool MPR121::begin(const Address address)
{
  address_ = address;
  return begin();
}

Error MPR121::getError()
{
  // important - this resets the IRQ pin - as does any I2C comms

  getRegister(OORS1); // OOR registers - we may not have read them yet,
  getRegister(OORS2); // whereas the other errors should have been caught

  // order of error precedence is determined in this logic block

  if (!isInitialized())
  {
    return NOT_INITIALIZED; // this has its own checker function
  }

  if (error_byte_ & (1<<ADDRESS_UNKNOWN_BIT))
  {
    return ADDRESS_UNKNOWN;
  }
  else if (error_byte_ & (1<<READBACK_FAIL_BIT))
  {
    return READBACK_FAIL;
  }
  else if (error_byte_ & (1<<OVERCURRENT_FLAG_BIT))
  {
    return OVERCURRENT_FLAG;
  }
  else if (error_byte_ & (1<<OUT_OF_RANGE_BIT))
  {
    return OUT_OF_RANGE;
  }
  else
  {
    return NO_ERROR;
  }
}

void MPR121::clearError()
{
  error_byte_ = 0;
}

bool MPR121::touchStatusChanged()
{
  // :: here forces the compiler to use Arduino's digitalRead, not MPR121's
  return (auto_touch_status_flag_ || (!::digitalRead(interrupt_pin_)));
}

void MPR121::updateTouchData()
{
  if (!isInitialized())
  {
    return;
  }

  auto_touch_status_flag_ = false;

  touch_data_previous_ = touch_data_;
  touch_data_ = (uint16_t)getRegister(TS1) + ((uint16_t)getRegister(TS2)<<8);
}

bool MPR121::updateBaselineData()
{
  if (!isInitialized())
  {
    return !SUCCESS;
  }

  wire_ptr_->beginTransmission(address);
  wire_ptr_->write(E0BV);   // set address register to read from the start of the
  // baseline data
  wire_ptr_->endTransmission(false); // repeated start

  if (touchStatusChanged())
  {
    auto_touch_status_flag_ = true;
  }

  if (wire_ptr_->requestFrom(address,(uint8_t)ELECTRODE_COUNT) == ELECTRODE_COUNT)
  {
    for (size_t electrode=0; electrode<ELECTRODE_COUNT; ++electrode)
    { // ELECTRODE_COUNT filtered values
      if(touchStatusChanged())
      {
        auto_touch_status_flag_ = true;
      }
      baseline_data_[electrode] = wire_ptr_->read()<<2;
    }
    return SUCCCESS;
  }
  else
  {
    // if we don't get back all 26 values we requested, don't update the BVAL values
    // and return !SUCCESS
    return !SUCCESS;
  }
}

bool MPR121::updateFilteredData()
{
  if (!isInitialized())
  {
    return !SUCCESS;
  }

  uint8_t LSB, MSB;

  wire_ptr_->beginTransmission(address);
  wire_ptr_->write(E0FDL); // set address register to read from the start of the
  //filtered data
  wire_ptr_->endTransmission(false); // repeated start

  if (touchStatusChanged())
  {
    auto_touch_status_flag_ = true;
  }

  if (wire_ptr_->requestFrom(address,(uint8_t)26)==26)
  {
    for(size_t electrode=0; electrode<ELECTRODE_COUNT; ++electrode)
    { // ELECTRODE_COUNT filtered values
      if(touchStatusChanged())
      {
        auto_touch_status_flag_ = true;
      }
      LSB = wire_ptr_->read();
      if(touchStatusChanged())
      {
        auto_touch_status_flag_ = true;
      }
      MSB = wire_ptr_->read();
      filtered_data_[electrode] = ((MSB << 8) | LSB);
    }
    return SUCCESS;
  }
  else
  {
    // if we don't get back all 26 values we requested, don't update the FDAT values
    // and return !SUCCESS
    return !SUCCESS;
  }
}

void MPR121::updateAll()
{
  updateTouchData();
  updateBaselineData();
  updateFilteredData();
}

bool MPR121::touched(const uint8_t electrode)
{
  if ((electrode >= ELECTRODE_COUNT) || !isInitialized())
  {
    return false; // avoid out of bounds behaviour
  }

  return (touch_data_>>electrode)&1;
}

uint8_t MPR121::getTouchCount()
{
  uint8_t touch_count = 0;

  if (isInitialized())
  {
    for (size_t electrode=0; electrode<ELECTRODE_COUNT; ++electrode)
    {
      if (touched(electrode))
      {
        ++touch_count;
      }
    }
  }

  return touch_count;
}

int MPR121::getBaselineData(const uint8_t electrode)
{
  if ((electrode >= ELECTRODE_COUNT) || !isInitialized())
  {
    return(0xFFFF); // avoid out of bounds behaviour
  }

  return baseline_data_[electrode];
}

int MPR121::getFilteredData(const uint8_t electrode)
{
  if ((electrode >= ELECTRODE_COUNT) || !isInitialized())
  {
    return(0xFFFF); // avoid out of bounds behaviour
  }

  return filtered_data_[electrode];
}

bool MPR121::isNewTouch(const uint8_t electrode)
{
  if ((electrode >= ELECTRODE_COUNT) || !isInitialized())
  {
    return false; // avoid out of bounds behaviour
  }
  return ((getPreviousTouchData(electrode) == false) && (touched(electrode) == true));
}

bool MPR121::isNewRelease(const uint8_t electrode)
{
  if ((electrode >= ELECTRODE_COUNT) || !isInitialized())
  {
    return false; // avoid out of bounds behaviour
  }
  return ((getPreviousTouchData(electrode) == true) && (touched(electrode) == false));
}

void MPR121::setTouchThreshold(const uint8_t threshold)
{
  if (!isInitialized())
  {
    return;
  }

  bool was_running = running_;

  if (was_running)
  {
    stop();  // can only change thresholds when not running
  }
  // checking here avoids multiple stop() / run()
  // calls

  for (size_t electrode=0; electrode<ELECTRODE_COUNT; ++electrode)
  {
    setTouchThreshold(electrode, threshold);
  }

  if (was_running)
  {
    run();
  }
}

void MPR121::setTouchThreshold(const uint8_t electrode, const uint8_t threshold)
{
  if ((electrode >= ELECTRODE_COUNT) || !isInitialized())
  {
    return; // avoid out of bounds behaviour
  }

  // this relies on the internal register map of the MPR121
  setRegister(E0TTH + (electrode<<1), threshold);
}

void MPR121::setReleaseThreshold(uint8_t threshold)
{
  if (!isInitialized())
  {
    return;
  }

  bool was_running = running;

  if(was_running)
  {
    stop();  // can only change thresholds when not running
  }
  // checking here avoids multiple stop / starts

  for (size_t electrode=0; electrode<ELECTRODE_COUNT; ++electrode)
  {
    setReleaseThreshold(electrode,threshold);
  }

  if (was_running)
  {
    run();
  }
}

void MPR121::setReleaseThreshold(uint8_t electrode, uint8_t threshold)
{
  if ((electrode >= ELECTRODE_COUNT) || !isInitialized())
  {
    return; // avoid out of bounds behaviour
  }

  // this relies on the internal register map of the MPR121
  setRegister(E0RTH + (electrode<<1), threshold);
}

uint8_t MPR121::getTouchThreshold(uint8_t electrode)
{
  if ((electrode >= ELECTRODE_COUNT) || !isInitialized())
  {
    return(0xFF); // avoid out of bounds behaviour
  }
  return getRegister(E0TTH+(electrode<<1)); // "255" issue is in here somewhere
}
uint8_t MPR121::getReleaseThreshold(uint8_t electrode)
{
  if ((electrode >= ELECTRODE_COUNT) || !isInitialized())
  {
    return(0xFF); // avoid out of bounds behaviour
  }
  return getRegister(E0RTH+(electrode<<1)); // "255" issue is in here somewhere
}

void MPR121::goSlow()
{
  wire_ptr_->setClock(100000L); // set I2C clock to 100kHz
}

void MPR121::goFast()
{
  wire_ptr_->setClock(400000L); // set I2C clock to 400kHz
}

void MPR121::setRegister(const uint8_t reg, const uint8_t value)
{

  bool was_running = false;;

  if (reg == ECR)
  { // if we are modding ECR, update our internal running status
    if (value&0x3F)
    {
      running_ = true;
    }
    else
    {
      running_ = false;
    }
  }
  else if (reg < CTL0)
  {
    was_running = running_;
    if (was_running)
    {
      stop();  // we should ALWAYS be in stop mode for this
    }
    // unless modding ECR or GPIO / LED register
  }

  wire_ptr_->beginTransmission(address_);
  wire_ptr_->write(reg);
  wire_ptr_->write(value);
  if (wire_ptr_->endTransmission()!=0)
  {
    error_byte_ |= 1<<ADDRESS_UNKNOWN_BIT; // set address unknown bit
  }
  else
  {
    error_byte_ &= ~(1<<ADDRESS_UNKNOWN_BIT);
  }

  if (was_running)
  {
    run();   // restore run mode if necessary
  }
}

uint8_t MPR121::getRegister(const uint8_t reg)
{
  uint8_t value;

  wire_ptr_->beginTransmission(address);
  wire_ptr_->write(reg); // set address to read from our requested register
  wire_ptr_->endTransmission(false); // repeated start
  wire_ptr_->requestFrom(address,(uint8_t)1);  // just a single byte
  if (wire_ptr_->endTransmission()!=0)
  {
    error_byte_ |= 1<<ADDRESS_UNKNOWN_BIT;
  }
  else
  {
    error_byte_ &= ~(1<<ADDRESS_UNKNOWN_BIT);
  }
  value = wire_ptr_->read();
  // auto update errors for registers with error data
  if (reg == TS2 && ((value&0x80)!=0))
  {
    error_byte_ |= 1<<OVERCURRENT_FLAG_BIT;
  }
  else
  {
    error_byte_ &= ~(1<<OVERCURRENT_FLAG_BIT);
  }
  if ((reg == OORS1 || reg == OORS2) && (value!=0))
  {
    error_byte_ |= 1<<OUT_OF_RANGE_BIT;
  }
  else
  {
    error_byte_ &= ~(1<<OUT_OF_RANGE_BIT);
  }
  return value;
}

void MPR121::run()
{
  if (!isInitialized())
  {
    return;
  }
  setRegister(ECR,ecr_backup_); // restore backup to return to run mode
}

void MPR121::stop()
{
  if(!isInitialized())
  {
    return;
  }
  ecr_backup_ = getRegister(ECR);  // backup ECR to restore when we enter run
  setRegister(ECR, ecr_backup_ & 0xC0); // turn off all electrodes to stop
}

bool MPR121::reset()
{
  // return SUCCESS if we successfully reset a device at the
  // address we are expecting

  // AFE2 is one of the few registers that defaults to a non-zero value -
  // checking it is sensible as reading back an incorrect value implies
  // something went wrong - we also check TS2 bit 7 to see if we have an
  // overcurrent flag set

  setRegister(SRST, 0x63); // soft reset

  if (getRegister(AFE2)!=0x24)
  {
    error_byte_ |= 1<<READBACK_FAIL_BIT;
  }
  else
  {
    error_byte_ &= ~(1<<READBACK_FAIL_BIT);
  }

  if ((getRegister(TS2)&0x80)!=0)
  {
    error_byte_ |= 1<<OVERCURRENT_FLAG_BIT;
  }
  else
  {
    error_byte_ &= ~(1<<OVERCURRENT_FLAG_BIT);
  }

  if (getError()==NOT_INITIALIZED || getError()==NO_ERROR)
  { // if our only error is that we are not initialized...
    return SUCCESS;
  }
  else
  {
    return !SUCCESS;
  }
}

bool MPR121::isRunning()
{
  return running_;
}

bool MPR121::isInitialized()
{
  return !(error_byte_ & (1<<NOT_INITIALIZED_BIT));
}

void MPR121::setInterruptPin(const uint8_t pin)
{
  // :: here forces the compiler to use Arduino's pinMode, not MPR121's
  if (!isInitialized())
  {
    return;
  }
  ::pinMode(pin,INPUT_PULLUP);
  interrupt_pin_ = pin;
}

void MPR121::setProximityMode(const ProximityMode mode)
{

  if (!isInitialized())
  {
    return;
  }

  bool was_running = running;

  if (was_running)
  {
    stop();
  }

  switch(mode)
  {
    case DISABLED:
    {
      ecr_backup_ &= ~(3<<4);  // ELEPROX[0:1] = 00
      break;
    }
    case PROX0_1:
    {
      ecr_backup_ |=  (1<<4);  // ELEPROX[0:1] = 01
      ecr_backup_ &= ~(1<<5);
      break;
    }
    case PROX0_3:
    {
      ecr_backup_ &= ~(1<<4);  // ELEPROX[0:1] = 10
      ecr_backup_ |=  (1<<5);
      break;
    }
    case PROX0_11:
    {
      ecr_backup_ |=  (3<<4);  // ELEPROX[0:1] = 11
      break;
    }
  }

  if (was_running)
  {
    run();
  }
}

void MPR121::setDigitalPinCount(const uint8_t pin_count)
{
  if(!isInitialized()) return;
  bool was_running = running;

  if (pin_count > DIGITAL_PIN_COUNT_MAX)
  {
    pin_count = DIGITAL_PIN_COUNT_MAX;
  }

  if (was_running)
  {
    stop(); // have to stop to change ECR
  }
  ecr_backup_ = (0x0F & ((ELECTRODE_COUNT - 1) - pin_count)) | (ecr_backup_ & 0xF0);
  if (was_running)
  {
    run();
  }

}

void MPR121::pinMode(const uint8_t electrode, const PinMode mode)
{

  // only valid for ELE4..ELE11
  if(electrode<4 || electrode >11 || !isInitialized()) return;

  // LED0..LED7
  uint8_t bitmask = 1<<(electrode-4);

  switch(mode){
    case INPUT_PU:
      // EN = 1
      // DIR = 0
      // CTL0 = 1
      // CTL1 = 1
      setRegister(EN, getRegister(EN) | bitmask);
      setRegister(DIR, getRegister(DIR) & ~bitmask);
      setRegister(CTL0, getRegister(CTL0) | bitmask);
      setRegister(CTL1, getRegister(CTL1) | bitmask);
      break;
    case INPUT_PD:
      // EN = 1
      // DIR = 0
      // CTL0 = 1
      // CTL1 = 0
      setRegister(EN, getRegister(EN) | bitmask);
      setRegister(DIR, getRegister(DIR) & ~bitmask);
      setRegister(CTL0, getRegister(CTL0) | bitmask);
      setRegister(CTL1, getRegister(CTL1) & ~bitmask);
      break;
    case OUTPUT_HS:
      // EN = 1
      // DIR = 1
      // CTL0 = 1
      // CTL1 = 1
      setRegister(EN, getRegister(EN) | bitmask);
      setRegister(DIR, getRegister(DIR) | bitmask);
      setRegister(CTL0, getRegister(CTL0) | bitmask);
      setRegister(CTL1, getRegister(CTL1) | bitmask);
      break;
    case OUTPUT_LS:
      // EN = 1
      // DIR = 1
      // CTL0 = 1
      // CTL1 = 0
      setRegister(EN, getRegister(EN) | bitmask);
      setRegister(DIR, getRegister(DIR) | bitmask);
      setRegister(CTL0, getRegister(CTL0) | bitmask);
      setRegister(CTL1, getRegister(CTL1) & ~bitmask);
      break;
  }
}

void MPR121::pinMode(uint8_t electrode, int mode)
{
  if(!isInitialized()) return;

  // this is to catch the fact that Arduino prefers its definition of INPUT
  // and OUTPUT to ours...

  uint8_t bitmask = 1<<(electrode-4);

  if(mode == OUTPUT){
    // EN = 1
    // DIR = 1
    // CTL0 = 0
    // CTL1 = 0
    setRegister(EN, getRegister(EN) | bitmask);
    setRegister(DIR, getRegister(DIR) | bitmask);
    setRegister(CTL0, getRegister(CTL0) & ~bitmask);
    setRegister(CTL1, getRegister(CTL1) & ~bitmask);

  } else if(mode == INPUT){
    // EN = 1
    // DIR = 0
    // CTL0 = 0
    // CTL1 = 0
    setRegister(EN, getRegister(EN) | bitmask);
    setRegister(DIR, getRegister(DIR) & ~bitmask);
    setRegister(CTL0, getRegister(CTL0) & ~bitmask);
    setRegister(CTL1, getRegister(CTL1) & ~bitmask);
  } else {
    return; // anything that isn't a 1 or 0 is invalid
  }
}

void MPR121::digitalWrite(uint8_t electrode, uint8_t val)
{

  // avoid out of bounds behaviour

  if(electrode<4 || electrode>11 || !isInitialized()) return;

  if(val){
    setRegister(SET, 1<<(electrode-4));
  } else {
    setRegister(CLR, 1<<(electrode-4));
  }
}

void MPR121::digitalToggle(uint8_t electrode)
{

  // avoid out of bounds behaviour

  if(electrode<4 || electrode>11 || !isInitialized()) return;

  setRegister(TOG, 1<<(electrode-4));
}

bool MPR121::digitalRead(uint8_t electrode)
{

  // avoid out of bounds behaviour

  if(electrode<4 || electrode>11 || !isInitialized()) return !SUCCESS;

  return(((getRegister(DAT)>>(electrode-4))&1)==1);
}

void MPR121::analogWrite(uint8_t electrode, uint8_t value)
{
  // LED output 5 (ELE9) and output 6 (ELE10) have a PWM bug
  // https://community.nxp.com/thread/305474

  // avoid out of bounds behaviour

  if(electrode<4 || electrode>11 || !isInitialized()) return;

  uint8_t shiftedVal = value>>4;

  if(shiftedVal > 0){
    setRegister(SET, 1<<(electrode-4)); // normal PWM operation
  } else {
    // this make a 0 PWM setting turn off the output
    setRegister(CLR, 1<<(electrode-4));
  }

  uint8_t scratch;

  switch(electrode-4){

    case 0:
      scratch = getRegister(PWM0);
      setRegister(PWM0, (shiftedVal & 0x0F) | (scratch & 0xF0));
      break;
    case 1:
      scratch = getRegister(PWM0);
      setRegister(PWM0, ((shiftedVal & 0x0F)<<4) | (scratch & 0x0F));
      break;
    case 2:
      scratch = getRegister(PWM1);
      setRegister(PWM1, (shiftedVal & 0x0F) | (scratch & 0xF0));
      break;
    case 3:
      scratch = getRegister(PWM1);
      setRegister(PWM1, ((shiftedVal & 0x0F)<<4) | (scratch & 0x0F));
      break;
    case 4:
      scratch = getRegister(PWM2);
      setRegister(PWM2, (shiftedVal & 0x0F) | (scratch & 0xF0));
      break;
    case 5:
      scratch = getRegister(PWM2);
      setRegister(PWM2, ((shiftedVal & 0x0F)<<4) | (scratch & 0x0F));
      break;
    case 6:
      scratch = getRegister(PWM3);
      setRegister(PWM3, (shiftedVal & 0x0F) | (scratch & 0xF0));
      break;
    case 7:
      scratch = getRegister(PWM3);
      setRegister(PWM3, ((shiftedVal & 0x0F)<<4) | (scratch & 0x0F));
      break;
  }
}

void MPR121::setSamplePeriod(SamplePeriod period)
{
  uint8_t scratch;

  scratch = getRegister(AFE2);
  setRegister(AFE2, (scratch & 0xF8) | (period & 0x07));
}

void MPR121::applySettings(const Settings & settings)
{
  bool was_running = running;
  if(was_running)
  {
    stop();  // can't change most regs when running - checking
  }
  // here avoids multiple stop() / run() calls

  setRegister(MHDR,settings->MHDR);
  setRegister(NHDR,settings->NHDR);
  setRegister(NCLR,settings->NCLR);
  setRegister(FDLR,settings->FDLR);
  setRegister(MHDF,settings->MHDF);
  setRegister(NHDF,settings->NHDF);
  setRegister(NCLF,settings->NCLF);
  setRegister(FDLF,settings->FDLF);
  setRegister(NHDT,settings->NHDT);
  setRegister(NCLT,settings->NCLT);
  setRegister(FDLT,settings->FDLT);
  setRegister(MHDPROXR,settings->MHDPROXR);
  setRegister(NHDPROXR,settings->NHDPROXR);
  setRegister(NCLPROXR,settings->NCLPROXR);
  setRegister(FDLPROXR,settings->FDLPROXR);
  setRegister(MHDPROXF,settings->MHDPROXF);
  setRegister(NHDPROXF,settings->NHDPROXF);
  setRegister(NCLPROXF,settings->NCLPROXF);
  setRegister(FDLPROXF,settings->FDLPROXF);
  setRegister(NHDPROXT,settings->NHDPROXT);
  setRegister(NCLPROXT,settings->NCLPROXT);
  setRegister(FDLPROXT,settings->FDLPROXT);
  setRegister(DTR, settings->DTR);
  setRegister(AFE1, settings->AFE1);
  setRegister(AFE2, settings->AFE2);
  setRegister(ACCR0, settings->ACCR0);
  setRegister(ACCR1, settings->ACCR1);
  setRegister(USL, settings->USL);
  setRegister(LSL, settings->LSL);
  setRegister(TL, settings->TL);

  setRegister(ECR, settings->ECR);

  error_byte_ &= ~(1<<NOT_INITIALIZED_BIT); // clear not inited error as we have just inited!
  setTouchThreshold(settings->TTHRESH);
  setReleaseThreshold(settings->RTHRESH);
  setInterruptPin(settings->INTERRUPT);

  if(was_running)
  {
    run();
  }
}

bool MPR121::getPreviousTouchData(const uint8_t electrode)
{
  if ((electrode > (ELECTRODE_COUNT - 1)) || !isInitialized())
  {
    return !SUCCESS; // avoid out of bounds behaviour
  }

  return ((touch_data_previous_>>electrode)&1);
}
