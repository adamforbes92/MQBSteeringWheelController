
# MQBSteeringWheelController
 For parsing MQB & PQ VW Steering Wheels and transmit over CAN.  Can be used to mix & match steering wheel types between variants; i.e., MQB wheel into PQ, or PQ wheel into MQB.

## Concept
The PQ and MQB steering wheels are controlled over LIN and require a 'backlight' frame to be sent at regular frequencies.  This keeps the steering wheel 'alive'.  It will NOT send data/button presses back without it.

> Send the LIN packet to wake the wheel up (with background lighting details, if applicable)
> Read button presses as they come in
> Parse the button press and translate into new ID for LIN & CAN
> Parse the button press for resistor value (for analogue radios)
> Send the new ID & set the resistor value

## Inputs
LIN 1 (from steering wheel buttons)
Analog backlight (PWM for MK4 platforms)

## Outputs
LIN 2 (to chassis, if req.)
CAN (to chassis, if req.)
Resistive value for analogue radios
