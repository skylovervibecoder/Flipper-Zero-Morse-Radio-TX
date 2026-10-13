# Morse Radio TX for Flipper Zero

A Flipper Zero application that allows you to transmit custom text or quick SOS messages in Morse code using the internal Sub-GHz radio module across PMR446 and LPD433 frequencies.

---

## Features

- **Text-to-Morse Conversion:** Type your custom text directly on the Flipper Zero interface and convert it into Morse code.
- **Quick SOS:** Send an emergency SOS Morse sequence instantly by pressing the down button.
- **Multiple Frequency Bands:** Supports PMR446 (channels 1–16) and LPD433 (channels 1–69).
- **Modulation Modes:** 
  - *FM Tone:* Continuous carrier with frequency modulation.
  - *OOK Tone:* On-Off Keying using an audio tone.
  - *CW (OOK):* Traditional Continuous Wave (OOK) pulsing.
- **Configurable Settings:** Adjustable transmission speed in WPM (Words Per Minute) and audio tone frequencies (500Hz, 700Hz, 1000Hz).

---

## How to Use

1. **Main Interface:**
   - Use the **Left** and **Right** arrow keys to select your desired frequency band and channel.
   - Press **OK** to transmit your current message, or if the text box is empty, it will prompt you to enter a custom message.
   - Press **Down** to instantly transmit an emergency **SOS** sequence.
   - Press **Up** to open the settings menu.

2. **Settings Menu:**
   - **Messaggio:** Opens the on-screen text input to change your transmission string.
   - **Banda:** Switch between `PMR446` and `LPD433`.
   - **Canale:** Select the specific channel index.
   - **Velocita WPM:** Adjust the transmission speed (from 5 WPM upwards).
   - **Modo:** Choose between *Tono FM*, *Tono OOK*, and *CW (OOK)*.
   - **Tono:** Select the audio frequency for the tone modes.

3. **During Transmission:**
   - The screen will display a progress bar indicating transmission status, and the Flipper Zero blue LED will remain solid blue.
   - Press the **Back** key at any time to abort the transmission.

---

## Disclaimer

**IMPORTANT:** This software is provided for educational, experimental, and amateur radio hobbyist purposes only. 

- Transmitting radio signals on restricted frequencies (such as PMR446 or LPD433 bands) without the proper licensing, authorization, or outside of legal regional power/regulatory limits may violate local telecommunications laws.
- The author and contributors assume **no liability** and are not responsible for any misuse, legal consequences, or interference caused by the use of this application. 
- Use this tool responsibly, respect local regulations, and ensure you only transmit on permitted frequencies in your jurisdiction.