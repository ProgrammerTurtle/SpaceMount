# SpaceMount

**SpaceMount** is an open source astrophotography mount suite consisting of an automatic polar alignment unit, an equatorial mount unit, and a custom carbon fiber guider scope. 

Control is carried out by a Raspberry Pi 4B running [PINS](https://github.com/nitr57/pins) talking to an SKR Pico 3D printer mainboard sporting custom firmware. 

## Structure

SpaceMount sports a high rigidity frame made out of 1/4" lasercut aluminum, aluminum extrusions, and printed parts made of engineering-grade materials. This allows for high load capacity, high rigidity, and high accuracy. All important features to get the perfect shot.

## Drive

Mechanically, it features 3 fully custom high ratio dual stage cycloidal gearboxes powered by high torque Nema 17 stepper motors.
- One Gearbox for Azimuth
- One Gearbox for DEC
- One Gearbox for R. A.
These gearboxes are an 81:1 reduction, outputting 35Nm of torque! This allows for a high load capacity. 

The altitude axis features a custom 200:1 worm-gear-belt-drive system for resisting the toughest of conditions. 

## Polar Alignment

Mounted on the altitude axis is a camera for Polar Alignment. Utilizing Plate Solving running on the Raspberry Pi 4B, it will automatically scan the sky and align your equatorial mount with Earth's pole! No fiddling with alignment for hours. 
