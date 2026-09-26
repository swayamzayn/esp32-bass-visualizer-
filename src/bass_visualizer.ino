#include <IRremote.hpp>
#include <FastLED.h>

// =========================
// PINS
// =========================
#define IR_RECEIVE_PIN 18
#define LED_PIN        23

// =========================
// LED SETUP
// =========================
#define NUM_LEDS 10

CRGB leds[NUM_LEDS];


// =========================
// VU / BASS DATA
// =========================
float leftVU  = 0.0f;
float rightVU = 0.0f;

float leftBass  = 0.0f;
float rightBass = 0.0f;

float targetLeftVU  = 0.0f;
float targetRightVU = 0.0f;

float targetLeftBass  = 0.0f;
float targetRightBass = 0.0f;


// =========================
// VU SMOOTHING
// =========================
const float VU_ATTACK_SPEED = 0.35f;
const float VU_DECAY_SPEED  = 0.06f;


// =========================
// BASS BAR BRIGHTNESS
// =========================

// Fixed idle glow.
// Remote brightness does NOT change this.
const int IDLE_BRIGHTNESS = 12;

// Maximum brightness during bass hits.
// Controlled by remote.
int peakBrightness = 255;

const int BRIGHTNESS_STEP = 20;
const int MIN_PEAK_BRIGHTNESS = 255;
const int MAX_PEAK_BRIGHTNESS = 255;


// =========================
// DISPLAY MODE
// =========================
//
// 0 = Bass Bar
// 1 = VU
//
int displayMode = 0;


// =========================
// PEAK HOLD
// =========================
//
// Used by VU mode.
//
struct PeakState {
  float peak;
  unsigned long lastPeakTime;
  unsigned long flashUntil;
};

PeakState leftPeak  = {0, 0, 0};
PeakState rightPeak = {0, 0, 0};

const unsigned long PEAK_FLASH_TIME = 100;
const unsigned long PEAK_HOLD_TIME  = 350;


// =========================
// SERIAL VU RECEIVER
// =========================
char serialBuffer[80];
uint8_t serialPos = 0;


// =========================
// TIMING
// =========================
unsigned long lastVUUpdate = 0;
unsigned long lastRender   = 0;

unsigned long lastBrightnessIR = 0;
const unsigned long IR_DEBOUNCE = 180;


// =========================
// VU SMOOTHING
// =========================

float smoothVU(float current, float target)
{
  if (target > current) {
    current += (target - current) * VU_ATTACK_SPEED;
  }
  else {
    current += (target - current) * VU_DECAY_SPEED;
  }

  if (current < 0.0f)
    current = 0.0f;

  if (current > 100.0f)
    current = 100.0f;

  return current;
}


// =========================
// VU LED BRIGHTNESS
// =========================

int calculateLEDValue(float vu, float bass, int ledLevel)
{
  float ledBottom = ledLevel * 20.0f;
  float ledTop    = ledBottom + 20.0f;

  float amount = 0.0f;

  if (vu >= ledTop) {
    amount = 1.0f;
  }
  else if (vu > ledBottom) {
    amount = (vu - ledBottom) / 20.0f;
  }
  else {
    amount = 0.0f;
  }

  if (amount <= 0.0f)
    return 0;

  float brightness =
    40.0f +
    amount * 150.0f;

  // Bass adds extra punch in VU mode.
  brightness += bass * 0.65f;

  if (brightness > 255.0f)
    brightness = 255.0f;

  return (int)brightness;
}


// =========================
// VU COLOR
// =========================

CHSV vuColor(int level)
{
  // Bottom = green
  // Middle = yellow
  // Top = red

  if (level <= 1) {
    return CHSV(96, 255, 255);
  }

  if (level == 2) {
    return CHSV(55, 255, 255);
  }

  if (level == 3) {
    return CHSV(35, 255, 255);
  }

  return CHSV(0, 255, 255);
}


// =========================
// PEAK UPDATE
// =========================

void updatePeak(PeakState &p, float vu)
{
  if (vu > p.peak + 2.0f) {

    p.peak = vu;
    p.lastPeakTime = millis();
    p.flashUntil = millis() + PEAK_FLASH_TIME;
  }

  if (millis() - p.lastPeakTime > PEAK_HOLD_TIME) {

    p.peak -= 0.8f;

    if (p.peak < 0.0f)
      p.peak = 0.0f;
  }
}


// =========================
// DRAW ONE VU BAR
// =========================

void drawBar(
  float vu,
  float bass,
  int bottomIndex,
  int direction,
  PeakState &peakState
)
{
  for (int level = 0; level < 5; level++) {

    int index;

    if (direction == 1) {
      index = bottomIndex + level;
    }
    else {
      index = bottomIndex - level;
    }

    int brightness =
      calculateLEDValue(
        vu,
        bass,
        level
      );

    if (brightness > 0) {

      leds[index] = vuColor(level);
      leds[index].nscale8(brightness);

    }
    else {

      leds[index] = CRGB::Black;

    }
  }


  // =========================
  // VU PEAK LED
  // =========================

  if (peakState.peak > 0.0f) {

    int peakLevel =
      (int)(peakState.peak / 20.0f);

    if (peakLevel > 4)
      peakLevel = 4;

    int peakIndex;

    if (direction == 1) {
      peakIndex = bottomIndex + peakLevel;
    }
    else {
      peakIndex = bottomIndex - peakLevel;
    }


    // Brief white flash
    if (millis() < peakState.flashUntil) {

      leds[peakIndex] = CRGB::White;

    }
    else if (
      millis() - peakState.lastPeakTime <
      PEAK_HOLD_TIME
    ) {

      leds[peakIndex] = CRGB::White;

    }
  }
}


// =========================
// RENDER VU
// =========================

void renderVU()
{
  // RIGHT BAR
  //
  // LEDs 0-4
  // LED 0 = bottom
  // LED 4 = top

  drawBar(
    rightVU,
    rightBass,
    0,
    1,
    rightPeak
  );


  // LEFT BAR
  //
  // LEDs 9-5
  // LED 9 = bottom
  // LED 5 = top

  drawBar(
    leftVU,
    leftBass,
    9,
    -1,
    leftPeak
  );


  FastLED.show();
}


// =========================
// BASS BAR BRIGHTNESS
// =========================
//
// Bass input = 0-100.
//
// Idle stays at IDLE_BRIGHTNESS.
//
// Strong bass approaches peakBrightness.
//
// The 2.2 power curve deliberately
// suppresses small bass values.
//
int calculateBassBrightness(float bass)
{
  float x =
    constrain(
      bass / 100.0f,
      0.0f,
      1.0f
    );


  // Strong nonlinear response.
  //
  // This is the important change:
  //
  // small bass -> stays dim
  // medium bass -> begins rising
  // strong bass -> rises sharply
  //
  x = powf(x, 4.2f);


  int brightness =
    IDLE_BRIGHTNESS +
    (int)(
      (peakBrightness - IDLE_BRIGHTNESS)
      * x
    );


  if (brightness < IDLE_BRIGHTNESS)
    brightness = IDLE_BRIGHTNESS;

  if (brightness > peakBrightness)
    brightness = peakBrightness;


  return brightness;
}


// =========================
// BASS COLOR
// =========================
//
// Pure blue family.
//
// Low level:
// deep saturated blue.
//
// Higher level:
// progressively lighter blue.
//
CHSV bassColor(float bass)
{
  float x =
    constrain(
      bass / 100.0f,
      0.0f,
      1.0f
    );


  // FastLED blue hue.
  uint8_t hue = 164;


  // Keep the low-level blue deep.
  //
  // As bass rises, saturation decreases
  // slightly, producing a lighter blue.
  //
  uint8_t saturation =
    255 -
    (uint8_t)(x * 30.0f);


  uint8_t value =
    calculateBassBrightness(bass);


  return CHSV(
    hue,
    saturation,
    value
  );
}


// =========================
// DRAW BASS BAR
// =========================

void drawBassBar(
  float bass,
  int bottomIndex,
  int direction
)
{
  CHSV color =
    bassColor(bass);


  // All five LEDs use the same
  // bass intensity.
  //
  // RIGHT:
  // 0 1 2 3 4
  //
  // LEFT:
  // 9 8 7 6 5

  for (int level = 0; level < 5; level++) {

    int index;

    if (direction == 1) {
      index = bottomIndex + level;
    }
    else {
      index = bottomIndex - level;
    }

    leds[index] = color;
  }
}


// =========================
// RENDER BASS BAR
// =========================

void renderBassBar()
{
  // =========================
  // RIGHT BAR
  // =========================

  drawBassBar(
    rightBass,
    0,
    1
  );


  // =========================
  // LEFT BAR
  // =========================

  drawBassBar(
    leftBass,
    9,
    -1
  );


  FastLED.show();
}


// =========================
// RECEIVE VU FROM PYTHON
// =========================
//
// Expected:
//
// VU:52,71,35,82
//
// L VU
// R VU
// L bass
// R bass
//
void processSerialLine(char *line)
{
  if (strncmp(line, "VU:", 3) != 0)
    return;


  int lVU;
  int rVU;
  int lBass;
  int rBass;


  int result =
    sscanf(
      line + 3,
      "%d,%d,%d,%d",
      &lVU,
      &rVU,
      &lBass,
      &rBass
    );


  if (result == 4) {

    targetLeftVU =
      constrain(
        lVU,
        0,
        100
      );

    targetRightVU =
      constrain(
        rVU,
        0,
        100
      );


    targetLeftBass =
      constrain(
        lBass,
        0,
        100
      );

    targetRightBass =
      constrain(
        rBass,
        0,
        100
      );
  }
}


// =========================
// SERIAL INPUT HANDLER
// =========================

void handleSerialInput()
{
  while (Serial.available()) {

    char c =
      Serial.read();


    if (c == '\n' || c == '\r') {

      if (serialPos > 0) {

        serialBuffer[serialPos] =
          '\0';

        processSerialLine(
          serialBuffer
        );

        serialPos = 0;
      }
    }

    else {

      if (
        serialPos <
        sizeof(serialBuffer) - 1
      ) {

        serialBuffer[serialPos++] =
          c;

      }

      else {

        // Prevent buffer overflow.
        serialPos = 0;

      }
    }
  }
}


// =========================
// IR HANDLER
// =========================

void handleIR()
{
  if (!IrReceiver.decode())
    return;


  // Ignore repeat frames.
  if (
    !(IrReceiver.decodedIRData.flags &
      IRDATA_FLAGS_IS_REPEAT)
  ) {

    uint16_t address =
      IrReceiver.decodedIRData.address;

    uint16_t command =
      IrReceiver.decodedIRData.command;


    // =====================================
    // LED BRIGHTNESS DOWN
    // Panasonic 08,11
    //
    // Controls MAXIMUM brightness.
    // Idle brightness stays unchanged.
    // =====================================

    if (
      address == 0x08 &&
      command == 0x11
    ) {

      if (
        millis() - lastBrightnessIR >
        IR_DEBOUNCE
      ) {

        peakBrightness -=
          BRIGHTNESS_STEP;


        if (
          peakBrightness <
          MIN_PEAK_BRIGHTNESS
        ) {

          peakBrightness =
            MIN_PEAK_BRIGHTNESS;
        }


        lastBrightnessIR =
          millis();
      }
    }


    // =====================================
    // LED BRIGHTNESS UP
    // Panasonic 08,10
    //
    // Controls MAXIMUM brightness.
    // =====================================

    else if (
      address == 0x08 &&
      command == 0x10
    ) {

      if (
        millis() - lastBrightnessIR >
        IR_DEBOUNCE
      ) {

        peakBrightness +=
          BRIGHTNESS_STEP;


        if (
          peakBrightness >
          MAX_PEAK_BRIGHTNESS
        ) {

          peakBrightness =
            MAX_PEAK_BRIGHTNESS;
        }


        lastBrightnessIR =
          millis();
      }
    }


    // =====================================
    // MODE TOGGLE
    //
    // Panasonic 98,A7
    //
    // 0 = Bass Bar
    // 1 = VU
    // =====================================

    else if (
      address == 0x98 &&
      command == 0xA7
    ) {

      displayMode++;


      if (displayMode > 1)
        displayMode = 0;


      if (displayMode == 0) {

        Serial.println(
          "MODE:BASS BAR"
        );

      }
      else {

        Serial.println(
          "MODE:VU"
        );
      }
    }


    // =====================================
    // YOUTUBE MUSIC LAUNCH
    //
    // Raw remote command:
    // 28,30
    //
    // Python expects:
    // 40,30
    // =====================================

    else if (
      address == 0x28 &&
      command == 0x30
    ) {

      Serial.println(
        "IR:40,30"
      );
    }


    // =====================================
    // EVERYTHING ELSE -> PYTHON
    // =====================================

    else {

      Serial.print("IR:");

      Serial.print(
        address,
        HEX
      );

      Serial.print(",");

      Serial.println(
        command,
        HEX
      );
    }
  }


  IrReceiver.resume();
}


// =========================
// SETUP
// =========================

void setup()
{
  Serial.begin(115200);


  // =========================
  // IR RECEIVER
  // =========================

  IrReceiver.begin(
    IR_RECEIVE_PIN,
    ENABLE_LED_FEEDBACK
  );


  // =========================
  // LEDS
  // =========================

  FastLED.addLeds<
    WS2812B,
    LED_PIN,
    GRB
  >(
    leds,
    NUM_LEDS
  );


  // Individual LED brightness
  // is handled manually.

  FastLED.setBrightness(255);


  FastLED.clear();
  FastLED.show();


  // =========================
  // INITIAL STATE
  // =========================

  leftVU = 0.0f;
  rightVU = 0.0f;

  targetLeftVU = 0.0f;
  targetRightVU = 0.0f;


  leftBass = 0.0f;
  rightBass = 0.0f;

  targetLeftBass = 0.0f;
  targetRightBass = 0.0f;


  displayMode = 0;

  peakBrightness = 255;


  Serial.println(
    "IR BASS BAR CONTROLLER READY"
  );
}


// =========================
// MAIN LOOP
// =========================

void loop()
{
  // =========================
  // PYTHON -> ESP32
  // =========================

  handleSerialInput();


  // =========================
  // REMOTE
  // =========================

  handleIR();


  // =========================
  // SMOOTHING
  // =========================

  unsigned long now =
    millis();


  if (
    now - lastVUUpdate >= 10
  ) {

    lastVUUpdate =
      now;


    // =========================
    // VU SMOOTHING
    // =========================

    leftVU =
      smoothVU(
        leftVU,
        targetLeftVU
      );


    rightVU =
      smoothVU(
        rightVU,
        targetRightVU
      );


    // =========================
    // BASS ATTACK / RELEASE
    // =========================
    //
    // Fast attack:
    // 25 ms
    //
    // Fast decay:
    // 150 ms
    //
    // This prevents the bar from
    // staying visually "full".
    //

    const float dt =
      0.010f;

    const float ATTACK_TIME =
      0.015f;

    const float RELEASE_TIME =
      0.35f;


    // =========================
    // LEFT BASS
    // =========================

    float leftTau;


    if (
      targetLeftBass >
      leftBass
    ) {

      leftTau =
        ATTACK_TIME;

    }
    else {

      leftTau =
        RELEASE_TIME;
    }


    float leftAlpha =
      1.0f -
      expf(
        -dt / leftTau
      );


    leftBass +=
      (
        targetLeftBass -
        leftBass
      ) *
      leftAlpha;


    // =========================
    // RIGHT BASS
    // =========================

    float rightTau;


    if (
      targetRightBass >
      rightBass
    ) {

      rightTau =
        ATTACK_TIME;

    }
    else {

      rightTau =
        RELEASE_TIME;
    }


    float rightAlpha =
      1.0f -
      expf(
        -dt / rightTau
      );


    rightBass +=
      (
        targetRightBass -
        rightBass
      ) *
      rightAlpha;


    // =========================
    // PEAK DETECTION
    // =========================
    //
    // Used only by VU mode.
    //

    updatePeak(
      leftPeak,
      leftVU
    );


    updatePeak(
      rightPeak,
      rightVU
    );
  }


  // =========================
  // LED REFRESH
  // =========================

  if (
    now - lastRender >= 16
  ) {

    lastRender =
      now;


    if (displayMode == 0) {

      renderBassBar();

    }
    else {

      renderVU();

    }
  }
}
