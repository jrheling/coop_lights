# coop_lights
Lighting controller to supplement natural light to ensure a fixed minimum amount of daily light for egg laying.

# Requirements
## Primary Objective
Supply supplemental lighting to winter chicken housing so that the combination of natural and supplemental lighting keeps the birds in light for X hours/day

## Additional Requirements
- light should never immediately turn off (chickens will be lost & confused)
- to minimize power consumption, don't light more than needed to hit the X hour goal (so supplemental lighting time should adapt to natural light cycles)

## Future Features
- supply a red-light-only "human maintenance mode" (operated by switch)
- wifi (use esp8266?)
- add temp/humidity sensor
- battery power + solar? (would need big battery)

# Hardware
* Arduino.  I'm using Sparkfun's Uno clone, but almost anything in the *duino family should work.
* 3-color LED strip.  (e.g. https://www.adafruit.com/products/346)
* MOSFETs to switch power into the LED strip (https://www.adafruit.com/product/355)
* Adequate power supply (depends on details of your chosen LED strip) (https://www.adafruit.com/products/352)
* Light sensitive resistor (https://www.adafruit.com/product/161)
* Real-time clock (https://www.adafruit.com/products/264)

# See Also
* http://www.thepoultrysite.com/articles/2820/new-studies-examine-effects-of-lighting-on-chickens/
* http://www.ledsmagazine.com/articles/print/volume-11/issue-5/features/agriculture/properties-of-led-light-can-boost-poultry-production-and-profits.html
* https://learn.adafruit.com/rgb-led-strips/overview
