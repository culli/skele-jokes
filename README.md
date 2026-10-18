# Skele Jokes

## Overview

This is the code and electronics to build two talking skeletons that take turns telling jokes with audio clips controlled over Bluetooth Low Energy (BLE), along with with flickering LED eyes. Each skeleton uses an Arduino Nano ESP32 controlling DY-SV5W MP3 module over UART.

Skeleton 1 starts a clip when its button is pressed or its infrared motion sensor detects activity. When the clip finishes, it sends the filename to skeleton 2, which plays its own recording with the same filename. Skeleton 1 waits for that reply to finish before accepting another trigger.

For example, `00001.mp3` can contain a joke setup on skeleton 1 and the punchline on skeleton 2. Audio stays on each skeleton's SD card; Bluetooth carries playback requests and acknowledgements.

I hope you have fun putting your own skeletons together! Mine are currently named Jerry Spinefeld and Nate Bonegatze, and they tell the worst skeleton dad jokes I could find. Props to the [Undertale community](https://steamcommunity.com/sharedfiles/filedetails/?id=537274165) for collecting so many groan-inducing jokes! 

## Disclaimer

This was written and designed in conjunction with Codex GPT-6 Astra. While I am a software engineer, I haven't written C code since university, so ChatGPT wrote the C code. I'm also not an electrical engineer, so I relied on the interwebs, youtube, and ChatGPT (checking its work as well as I could). I have done some hobby robotics with Raspberry Pi 3, DFRobotics components, and RasPi Robot board using Python. Rasbperry Pi seemed like overkill for this skeleton project, so I went with an Arduino Nano ESP32 instead for more of an embedded approach.

The links in this document are not affiliate links, I don't make anything from you buying these parts. They're just examples, soure the parts however you prefer.

## Tools

- Soldering station. I used one with an adjustable temperature at 360ºC, clamp holding arms, and a lighted magnifying glass. This was much better than my old single-temp soldering iron that I tried at first.
- 99.3% tin/0.7% copper 0.8mm solder wire. If it has flux in it, it will be easier to use.
- Flux pen if needed
- See many videos out there on soldering. I'm pretty novice at it and got this project working, so you can too!
- Wire strippers [Kobalt strippers at Lowes](https://www.amazon.com/K-balt-strippers-10-18-Solid-Stranded/dp/B0F9DTSVK9)
- Small "precision" screwdrivers
- [Hands free magnifier](https://www.amazon.com/Headband-Magnifier-Light%EF%BC%8CRechargeable%EF%BC%8CGlasses-Close-Lenses/dp/B0C3HK4ZQR) so you too can look silly. My eyes are aging and this is essential to read the text on the Nano and other electronics.

## Hardware

For **each skeleton**:

- Skeleton body: Home store skeletons. The ones I found had red LED eyes, an on/off switch, and no other electronics. They take 3x1.5v batteries and have an on-off switch. It was a lucky find that they are actually designed to hold more electronics, so there's plenty of room in the head for ths project.  The head comes open with a "few" screws and includes a place to mount a speaker, a movable jaw (once two screws were removed, it even has a lever arm which could be connected to a motor), and the LED eyes wired in. [5FT LED Pose-n-stay Skeleton at Home Depot](https://www.homedepot.com/p/Home-Accents-Holiday-5-FT-LED-Pose-N-Stay-Skeleton-5349-60272HD/205828003)
- `Arduino Nano ESP32`. [Nano on Amazon](https://www.amazon.com/gp/product/B0C947BHK5)
- Power source. This can be a 5V 2A **charger cable**. I wouldn't use a modern phone charger as those can deliver a lot more amps, and I don't know that a Nano can negotiate the power delivery.  *Or* you could use a **small USB power bank**, like for hand warmers or phones. Must be 5V and about 2A output. Some hand warmer systems have a higher voltage which would not work. [Canbory power bank on Amazon](https://www.amazon.com/gp/product/B0F436NDV9) Note: when I ordered these, one came and was bulging, so I got a replacement from the seller and recycled the damaged one.
- `DY-SV5W MP3` module configured for UART serial control. 1-OFF, 2-OFF, 3-ON [DY-SV5W MP3 on Amazon](https://www.amazon.com/gp/product/B0B1LDKYMK)
- Header pin breakaway headers. [20 Pcs 2.54mm 0.1in headers](https://www.amazon.com/gp/product/B0FRZW75VS)
- Compatible microSD card containing the audio clips. 32GB or less is all the DY-SV5W can handle. There are a lot of fake or bad SD cards out there, check [Matt Cole's database of cards](https://www.bahjeez.com/the-great-microsd-card-survey/).
- Speaker connected to the MP3 module's speaker terminals. My build uses a 4Ω, 3W speaker; choose volume to suit the speaker. [Gikfun 2" speaker](https://www.amazon.com/gp/product/B01CHYIU26)
- Momentary push button. [Tactile switches on Digikey](https://www.digikey.com/en/products/detail/te-connectivity-alcoswitch-switches/1825910-6/1632536)
- Two bare LED eyes. This build retains their existing parallel wiring and uses one shared 1 kΩ resistor at low current. In my case they came from the Home Depot skeleton.
- Breadboard. [Chanzon 6pc mini breadboard](https://www.amazon.com/gp/product/B07LF71ZTS) These don't have very tight connections, I'll try something else next project.
- Breadboard jumper wires. [360 piece jumper wires kit](https://www.amazon.com/360-Pc-Multicolor-Breadboard-Jumper-Wires/dp/B089FZ79CS) There are smaller kits out there, this is just to show the type of push-on wires that are great for prototyping. 8 and 4 inch, different combinations of ends.

Skeleton 1 also uses an **HC-SR501 PIR motion sensor**. [HC-SR-501 on digikey](https://www.digikey.com/en/products/detail/sunfounder/ST0012/22116811) If your model has a jumper, set it to H (high), mine is hard wired to H (tiny trace wire from center pad to H pad).

Skeleton 2 has no PIR sensor. The Nano's built-in BLE radio handles communication; no additional radio or cable between skeletons is required.

## Wiring

Use the **printed Arduino pin labels** (`D2`, `D3`, etc.). Disconnect power before changing connections.

### Connections on both skeletons

| Nano ESP32 | Connection | Notes |
| --- | --- | --- |
| `VBUS` | DY-SV5W `5V+` / 5 V input, when the Nano is USB-powered | label on opposite side of board |
| `GND` | DY-SV5W power negative (`5V−`) and signal `GND`, if separately exposed | Use common GND on breadboard |
| `D3` | DY-SV5W `TX` — receives player status and filename replies | |
| `D4` | DY-SV5W `RX` — sends playback commands | |
| `D2` | One button terminal; connect the other terminal to `GND` | |
| `3V3` | Through **one 1 kΩ resistor** to the LED pair's red/positive wire | |
| `D6` | LED pair's black/negative wire | |

Connect the speaker across the DY-SV5W's two speaker output terminals. Neither speaker terminal goes to Nano GND. Solder the wires to the speaker connectors, either by bending the push in wire (easy to break), disassembling the push-in wire end, or cutting and stripping the end. The MP3 module and Nano need a common ground, and **both UART wires are required**. UART runs at **9600 baud, 8N1**. I used a row on the breadboard as a common ground for the nano, mp3 board, sensor, and button. The mini-breadboards I used didn't have a dedicated ground section like a bigger breadboard would.

This wiring powers the MP3 module from the Nano's USB-derived `VBUS`; the module does not need a separate USB power connection. Nano GPIO signals use 3.3 V.

### Motion sensor on skeleton 1 only

| HC-SR501 | Nano ESP32 |
| --- | --- |
| `VCC` | `VBUS` |
| `GND` | `GND` |
| `OUT` | `D5` |

Follow the labels on the sensor rather than assuming a connector orientation. Leave `D5` unused on skeleton 2.

### LED eyes with the existing two-wire harness

```text
Nano 3V3 ---- 1 kΩ resistor ---- existing RED wire
Nano D6 ----------------------- existing BLACK wire
```

Leave the wiring between the two LEDs intact. Disconnect the original AA battery box and its resistor. **The black wire goes to D6, not GND; the supply is 3V3, not VBUS.** No transistor is needed for this low-current arrangement. Brown–black–red resistor bands indicate 1 kΩ. I disassembled the end of some of the push on wires by removing the plastic housing then soldered the resistor onto the red wire: LED red -> resistor -> push on wire -> nano 3v3 pin. For the black wire, I soldered another modified push wire onto it. You should probably insulate the connections with electrical tape or shrink tube.

The resistor limits the pair's total current to a few milliamps. Parallel LEDs may differ in brightness; separate resistors give more predictable current sharing. Keep the shared resistor at 1 kΩ for this setup. See [the detailed eye wiring notes](SKELETON-EYES.txt) for individual-resistor and transistor alternatives.

Both eyes flicker together while their own player reports playback. This is a timed animation, not audio-level or syllable tracking. The eyes turn off when playback stops; a loss of player status also turns them off after eight seconds.

One tip I found was to sand the LEDs a bit to make the light more diffuse for the eyes. [@mikecomptech youtube](https://m.youtube.com/watch?v=kvAXDp4FMU8&ra=m)

## Prepare the audio cards

Use matching short filenames on both cards, with different audio content for each skeleton:

```text
Skeleton 1 card       Skeleton 2 card
00001.mp3             00001.mp3
00002.mp3             00002.mp3
00003.mp3             00003.mp3
```

- Place one copy of each file in the card's root directory.
- Use a 1–8 character base name and an `.mp3`. Numbered names are simplest; matching ignores case.
- Avoid long filenames: the player reports FAT short aliases, which can differ between cards.
- Reserve leading `_` and `._` names for metadata. The sketches skip recognized metadata entries, including macOS sidecars, when filename queries succeed. `dot_clean` utility is your friend if you're loading the cards on a mac because the metadata files can cause problems on the mp3 board.  The code attempts to ignore them.
- The MP3 module supplies the file count. Adding clips does not require editing a filename list in the sketch, it should adapt to however many files you put on there, within reason.  
- Local playback follows the module's storage index order, which may differ from alphabetical order. The two cards do not need the same index order.
- Restart both boards after changing card contents to refresh counts and filename caches.

The player's reported entry count can include metadata files. Bluetooth replies require a verified matching filename; a missing match or failed query is reported as a failure rather than intentionally substituting another clip.

## Upload and first run

Note: I was unable to upload when both boards were connected. For debugging you can have both connected and see their serial port output. You may need to set `Tools > Programmer > Esptool`.

1. Install the **Arduino ESP32 Boards** package in Arduino IDE and select **Arduino Nano ESP32**. The classic AVR Nano board selection will not compile these sketches.
2. Open [motion_mp3/motion_mp3.ino](motion_mp3/motion_mp3.ino) and upload it to **skeleton 1**.
3. Open [button_mp3/button_mp3.ino](button_mp3/button_mp3.ino) and upload it to **skeleton 2**.
4. Keep all four headers beside each `.ino`: `SkeletonBLE.h`, `SkeletonProtocol.h`, `SkeletonTurn.h`, and `SkeletonEyes.h`. Open the existing sketch folders instead of copying only the `.ino` text into an empty sketch.
5. Power both skeletons. Either startup order works. No phone pairing or Wi-Fi configuration is needed.
6. Open Serial Monitor at **115200 baud**. Skeleton 1 should report `BLE: skeleton 2 connected.`; skeleton 2 should report `BLE: skeleton 1 connected.`
7. Press skeleton 1's button. It plays a clip, then skeleton 2 plays the matching filename. Check `Filename reported after PLAY` to identify the selected clip; the first one is not necessarily `00001.mp3`.

BLE and Preferences libraries are included with the board package. The build target is `arduino:esp32:nano_nora`. You can upload one board at a time, then power each independently for operation; an open Serial Monitor is not required.

The PIR sensor warms up for **60 seconds**. Button playback becomes available once the file count is known. After warmup, a sustained HIGH can trigger another clip when the turn is available; there is no requirement for the sensor to go LOW between turns. The two-second motion gap is measured from skeleton 1's clip ending, and waiting for skeleton 2 also blocks motion playback.

## Buttons and turn-taking

| Action or condition | Behavior |
| --- | --- |
| Skeleton 1 button while idle | Play the next usable entry |
| Skeleton 1 button during its own clip | Skip to the next entry; the interrupted clip does not request a reply |
| Skeleton 1 waiting for skeleton 2 | Block new local button and motion playback |
| Skeleton 2 button with no remote reply pending | Play or skip to its next local entry |
| Remote request arrives during skeleton 2's local clip | Queue the reply until that clip finishes |
| Skeleton 2 has a queued/playing remote reply | Block button interruption of the reply |
| Skeleton 1 reports a BLE timeout | Pause motion; press its button after checking skeleton 2 to retry the same request |

The first filename lookup on skeleton 2 may take several seconds. A cache accelerates later lookups, with verification before playback. Requests are retried with duplicate suppression; the receiver saves request state so retrying a completed request does not play the reply again.

See [the Bluetooth notes](SKELETON-BLUETOOTH.txt) for acknowledgement handling, recovery, and detailed timeout behavior.

## Increase quiet recordings

[scripts/normalize-audio.sh](scripts/normalize-audio.sh) measures loudness and creates a louder MP3 only when needed. It requires **Bash, Python 3, and FFmpeg with `libmp3lame`**.

```bash
# Measure and, if needed, create 00001-louder.mp3 beside the input.
bash normalize-audio.sh "00001.mp3"

# Preserve a matching short filename in an existing output directory.
mkdir -p normalized
bash normalize-audio.sh "00001.mp3" "normalized/00001.mp3"
```

The default target is **−16 LUFS**, with processing below −16.5 LUFS and a **−1.5 dBTP** limiter ceiling before encoding. Output is stereo, 44.1 kHz, 192 kbps MP3. The script measures the encoded result before saving and never overwrites the original or an existing destination. Already-loud-enough, silent, or unmeasurable files produce no new file.

Use the explicit output-name form for paired clips; the default `-louder` suffix can exceed the short-filename limit. Copy the chosen result to the appropriate SD card under its matching filename.

## Configuration

| Setting | Location | Default |
| --- | --- | --- |
| Player volume | `MP3_VOLUME` in each `.ino` | `30` |
| PIR warmup | `PIR_WARMUP_MS` in `motion_mp3.ino` | `60000` ms |
| Motion gap after skeleton 1's clip | `MOTION_GAP_MS` in `motion_mp3.ino` | `2000` ms |
| Periodic motion logs | `PIR_DIAGNOSTICS` in `motion_mp3.ino` | `true` |
| Eye effect | `EFFECT` in `SkeletonEyes.h` | `FLICKER`; also `BLINK` or `STEADY` |
| Eye output polarity | `ACTIVE_LOW` in `SkeletonEyes.h` | `true` for the wiring above |
| BLE delivery / completion timeouts | `SkeletonTurn.h` | 30 seconds / 15 minutes |
| Pair discovery identifier | `SERVICE_UUID` in `SkeletonBLE.h` | Shared by both boards |

Keep the four shared headers identical in both sketch folders. For another independent pair nearby, assign that pair a different service UUID in both copies.

## Diagnostics and troubleshooting

Send **`d`** in either board's Serial Monitor to display its **saved startup diagnostics**. This is a snapshot, not a fresh scan. It includes player state, device/count replies, and sampled indices and short filenames. There is no documented firmware-version query used by this project.

| Symptom | Check |
| --- | --- |
| `HardwareSerial::HardwareSerial(int)` compilation error | Select **Arduino Nano ESP32**, not an AVR Nano board |
| `No DFU capable USB device available` during upload | Check the selected board, USB data cable, and upload port; the board may need bootloader recovery |
| No track count or playback confirmation | Check MP3 power, UART control mode, common GND, and especially MP3 `TX` → Nano `D3` |
| Bluetooth connects, but filename lookup times out | Inspect **skeleton 2's** startup report and UART wiring; BLE connection alone does not establish communication with its MP3 module |
| Names look like `00005   MP3` | This is the module's space-padded short filename; the sketch decodes it as `00005.MP3` |
| Entry count exceeds the number of clips | Check for macOS metadata files; counts can include entries that playback filtering skips |
| Motion does not start another clip | Check PIR warmup, `OUT` → `D5`, player status, and whether skeleton 2's turn or a fault is blocking playback |
| Quiet audio | Check player volume, onboard volume control, and the recording's loudness; try the normalization script |
| Eyes stay off | Check LED polarity, the series 1 kΩ resistor, `3V3` supply, and the negative lead on `D6` |

Remote index/name queries allow three attempts of one second each. These retries repeat the query, not the playback command. Logs identify which command timed out; a timeout by itself does not prove a command is unsupported.

## Development and checks

The host simulations require **Python 3 and `clang++`** with C++17 and AddressSanitizer/UndefinedBehaviorSanitizer support. Run from the repository root:

```bash
python3 motion_mp3/tests/run_simulation.py
python3 button_mp3/tests/run_simulation.py
python3 tests/run_link_protocol.py
```

They cover button/PIR behavior, playback confirmation, metadata skipping, filename matching, BLE requests and retries, receiver restart recovery, and eye animation. The protocol check also verifies that both copies of each shared header match. Bluetooth and UART are mocked; use the first-run procedure to check physical wiring and radio operation.

With Arduino CLI installed and the Nano ESP32 board package available:

```bash
arduino-cli compile --fqbn arduino:esp32:nano_nora motion_mp3
arduino-cli compile --fqbn arduino:esp32:nano_nora button_mp3
```

## Project files

| Path | Purpose |
| --- | --- |
| [motion_mp3/](motion_mp3/) | Skeleton 1: PIR, button, MP3 playback, BLE client, and eyes |
| [button_mp3/](button_mp3/) | Skeleton 2: button, matching replies, BLE server, and eyes |
| [tests/](tests/) | Shared simulation support and protocol checks |
| [normalize-audio.sh](normalize-audio.sh) | Measure and increase quiet audio recordings |
| [SKELETON-BLUETOOTH.txt](SKELETON-BLUETOOTH.txt) | Detailed pairing, filename matching, and recovery notes |
| [SKELETON-EYES.txt](SKELETON-EYES.txt) | LED wiring options and component notes |
