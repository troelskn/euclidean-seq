# Simple, but functioning Euclidean Rhythm sequencer for RP2040.

#  Developed with

- Adafruit Feather RP2040
- Adafruit MIDI FeatherWing Kit
- Adafruit FeatherWing OLED - 128x64
- NeoKey 1x4 QT I2C - Four Mechanical Key Switches with NeoPixels - STEMMA QT / Qwiic

Connect OLED and Neokey with STEMMA QT / Qwiic cables.

See demo at: [https://www.reddit.com/r/synthdiy/comments/10susyk/euclidean_rhythms_sequencer_on_an_rp2040/](https://www.reddit.com/r/synthdiy/comments/10susyk/euclidean_rhythms_sequencer_on_an_rp2040/)

## Usage

- First button changes track
- Second button changes cursor
- Third button decrese value
- Fourth button increase value

There is a global menu on top. Use button 4 to enter. Clicking button 3 on the menu will mute current track.

You can select between internal clock, stopped and external clock. If external clock is selected, the sequencer will sync to midi-in clock.

## License

I hereby make this available under MIT license.
