#include <TFT_eSPI.h>
#include <SPI.h>

// Pure sprite-to-physical mapping calibration: one solid color pinned to each of the
// four sprite corners, no rotation/projection/anamorphic math involved at all. Report
// which color appears in which physical corner to derive the exact transform needed.

#define LCD_BL 20

TFT_eSPI tft = TFT_eSPI(240, 240);
TFT_eSprite sprite = TFT_eSprite(&tft);

const int16_t screenSize = 240;
const int16_t markerSize = 60;  // corner square side, in pixels

void setup() {
  Serial.begin(115200);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, 1);

  tft.begin();
  tft.invertDisplay(1);
  tft.setRotation(4);
  // Same panel mirror correction established for this board — see README.md
  // "Display is mirrored". Unrelated to the prism-stand viewing-angle question this
  // sketch is calibrating.
  // Color order forced to BGR (overriding User_Setup.h's TFT_RGB_ORDER=TFT_RGB):
  // yellow rendered as cyan with green unaffected is the signature of a red/blue
  // channel swap, so this panel needs BGR regardless of what the library assumes.
  tft.writecommand(TFT_MADCTL);
  tft.writedata(TFT_MAD_MX | TFT_MAD_BGR);
  tft.fillScreen(TFT_BLACK);

  sprite.createSprite(screenSize, screenSize);
  sprite.setColorDepth(16);

  sprite.fillSprite(TFT_BLACK);
  // First pass showed sprite-space maps to the physically-viewed image rotated 180
  // degrees (each sprite corner landed on its diagonal opposite). Colors are now drawn
  // pre-rotated 180 degrees here so they land at their PHYSICALLY intended corner.
  sprite.fillRect(screenSize - markerSize, screenSize - markerSize, markerSize, markerSize, TFT_RED);    // -> physical top-left
  sprite.fillRect(0, screenSize - markerSize, markerSize, markerSize, TFT_GREEN);                        // -> physical top-right
  sprite.fillRect(screenSize - markerSize, 0, markerSize, markerSize, TFT_BLUE);                         // -> physical bottom-left
  sprite.fillRect(0, 0, markerSize, markerSize, TFT_YELLOW);                                             // -> physical bottom-right
  sprite.pushSprite(0, 0);

  Serial.println("Colors pre-rotated 180deg to compensate for the prism stand's viewing angle.");
  Serial.println("Expected: RED=top-left, GREEN=top-right, BLUE=bottom-left, YELLOW=bottom-right.");
  Serial.println("Report which color is in which PHYSICAL corner as you view the board.");
}

void loop() {
  delay(1000);
}
