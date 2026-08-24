# SpaceMount

<img width="1935" height="1958" alt="Full_Astrophotography_Mount_2026-Aug-24_03-28-08AM-000_CustomizedView19236172479" src="https://github.com/user-attachments/assets/8a799fa4-fffc-45d8-b0b3-baf0695570d4" />

**SpaceMount** is an open source astrophotography mount suite consisting of an automatic polar alignment unit, an equatorial mount unit, and a custom carbon fiber guider scope. 

Control is carried out by a Raspberry Pi 4B running [PINS](https://github.com/nitr57/pins) talking to an SKR Pico 3D printer mainboard sporting custom firmware. 

## Structure

SpaceMount sports a high rigidity frame made out of 1/4" lasercut aluminum, aluminum extrusions, and printed parts made of engineering-grade materials. This allows for high load capacity, high rigidity, and high accuracy. All important features to get the perfect shot.

## Drive

<img width="4000" height="1305" alt="Gearbox_-_Lower_Stage_101_2026-Aug-24_03-46-59AM-000_CustomizedView28774878801" src="https://github.com/user-attachments/assets/475c21b1-70d2-4fd1-8adb-6b8504e60542" />

Mechanically, it features 3 fully custom high ratio dual stage cycloidal gearboxes powered by high torque Nema 17 stepper motors.
- One Gearbox for Azimuth
- One Gearbox for DEC
- One Gearbox for R. A.
These gearboxes are an 81:1 reduction, outputting 35Nm of torque! This allows for a high load capacity. 

The altitude axis features a custom 200:1 worm-gear-belt-drive system for resisting the toughest of conditions. 

## Polar Alignment

<img width="1189" height="896" alt="image" src="https://github.com/user-attachments/assets/30eac649-85f1-4935-9063-61b8abfdf24e" />

Mounted on the altitude axis is a camera for Polar Alignment. Utilizing Plate Solving running on the Raspberry Pi 4B, it will automatically scan the sky and align your equatorial mount with Earth's pole! No fiddling with alignment for hours. 

## Bill of Materials

The BOM can be found [here](https://docs.google.com/spreadsheets/d/1fLNac8KSqefRcGDmUohNeKdPQae2YU7nyDo7UR2l_qA/edit?usp=sharing) or attached as a .CSV in the repo. 
