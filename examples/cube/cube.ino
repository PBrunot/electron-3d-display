#include <TFT_eSPI.h>
#include <SPI.h>

#define LCD_BL 20

TFT_eSPI tft = TFT_eSPI(240, 240);
TFT_eSprite sprite = TFT_eSprite(&tft);

const int16_t screenSize = 240;
const int16_t centerX = screenSize / 2;
const int16_t centerY = screenSize / 2;
const float cubeScale = 100.0f;  // projected cube half-size, in pixels
const float camDistance = 2.8f;  // perspective camera distance, in cube half-widths

// 8 vertices of a unit cube, centered at the origin
const float cubeVertices[8][3] = {
  {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
  {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}
};

// 6 faces, each 4 vertex indices, wound CCW as seen from outside the cube (verified by
// hand via the right-hand rule against each face's known outward axis direction).
const uint8_t cubeFaces[6][4] = {
  {0,3,2,1},  // back  (z = -1)
  {4,5,6,7},  // front (z = +1)
  {0,4,7,3},  // left  (x = -1)
  {1,2,6,5},  // right (x = +1)
  {0,1,5,4},  // bottom (y = -1)
  {3,7,6,2},  // top    (y = +1)
};

const uint8_t faceColorsRGB[6][3] = {
  {255,0,0}, {0,255,0}, {0,0,255}, {255,255,0}, {255,0,255}, {0,255,255}
};

// Depth-shading range, in rotated-Z units (more negative = closer to the camera, per
// the projection formula below). A unit cube's rotated corners reach roughly +-1.73,
// so this comfortably covers the full range a face's average Z can take.
const float zNear = -1.8f;
const float zFar = 1.8f;
const float minBrightness = 0.35f;  // floor so far faces dim instead of vanishing to black

float angleX = 0, angleY = 0, angleZ = 0;

// Signed area of the 2D triangle (shoelace formula). A consistently-wound face is
// visible exactly when this has one particular sign, regardless of how strong the
// perspective distortion is at that point on screen — unlike a 3D normal dot-product
// test, which is only a paraxial approximation and visibly breaks down near the
// silhouette when the camera is close relative to the object (as it is here:
// camDistance=2.8 vs. cube half-size=1). This is the same technique TFT_eSPI's own
// bundled SpriteRotatingCube example uses for exactly this reason.
float signedArea(float x0, float y0, float x1, float y1, float x2, float y2) {
  return x0*y1 - y0*x1 + x1*y2 - y1*x2 + x2*y0 - y2*x0;
}

void setup() {
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, 1);

  tft.begin();
  tft.invertDisplay(1);
  tft.setRotation(4);
  // This panel's column scan direction is mirrored relative to what MADCTL's MX bit
  // normally means; setRotation() alone can't fix it since all 4 of its presets are
  // proper rotations that preserve mirroring. Raw MADCTL write with just MX cancels it.
  // Color order forced to BGR (overriding User_Setup.h's TFT_RGB_ORDER=TFT_RGB): a
  // red/blue channel swap was confirmed via the corner_calibration test sketch.
  // See README.md "Display is mirrored" for the full derivation.
  tft.writecommand(TFT_MADCTL);
  tft.writedata(TFT_MAD_MX | TFT_MAD_BGR);
  tft.fillScreen(TFT_BLACK);

  sprite.createSprite(screenSize, screenSize);
  sprite.setColorDepth(16);
}

void loop() {
  sprite.fillSprite(TFT_BLACK);

  float sx = sin(angleX), cx = cos(angleX);
  float sy = sin(angleY), cy = cos(angleY);
  float sz = sin(angleZ), cz = cos(angleZ);

  float projected[8][2];
  float vertexZ[8];

  for (int i = 0; i < 8; i++) {
    float x = cubeVertices[i][0];
    float y = cubeVertices[i][1];
    float z = cubeVertices[i][2];

    // Rotate around X axis
    float y1 = y * cx - z * sx;
    float z1 = y * sx + z * cx;
    // Rotate around Y axis
    float x2 = x * cy + z1 * sy;
    float z2 = -x * sy + z1 * cy;
    // Rotate around Z axis
    float x3 = x2 * cz - y1 * sz;
    float y3 = x2 * sz + y1 * cz;

    // Perspective projection
    float factor = cubeScale / (z2 + camDistance);
    // Both axes negated: the corner_calibration test confirmed the prism stand's 45deg
    // mirror presents sprite space to the viewer rotated 180 degrees. No anamorphic
    // stretch is applied — a planar mirror reflection is isometric, it repositions the
    // image but never stretches it.
    projected[i][0] = centerX - x3 * factor;
    projected[i][1] = centerY - y3 * factor;
    vertexZ[i] = z2;
  }

  for (int f = 0; f < 6; f++) {
    const uint8_t *v = cubeFaces[f];
    int16_t x0 = projected[v[0]][0], y0 = projected[v[0]][1];
    int16_t x1 = projected[v[1]][0], y1 = projected[v[1]][1];
    int16_t x2 = projected[v[2]][0], y2 = projected[v[2]][1];
    int16_t x3 = projected[v[3]][0], y3 = projected[v[3]][1];

    if (signedArea(x0, y0, x1, y1, x2, y2) >= 0) continue;  // back-facing

    float avgZ = (vertexZ[v[0]] + vertexZ[v[1]] + vertexZ[v[2]] + vertexZ[v[3]]) * 0.25f;
    float fraction = (zFar - avgZ) / (zFar - zNear);
    fraction = constrain(fraction, 0.0f, 1.0f);
    float brightness = minBrightness + (1.0f - minBrightness) * fraction;
    uint16_t shadedColor = sprite.color565(
      (uint8_t)(faceColorsRGB[f][0] * brightness),
      (uint8_t)(faceColorsRGB[f][1] * brightness),
      (uint8_t)(faceColorsRGB[f][2] * brightness));

    sprite.fillTriangle(x0, y0, x1, y1, x2, y2, shadedColor);
    sprite.fillTriangle(x0, y0, x2, y2, x3, y3, shadedColor);

    sprite.drawLine(x0, y0, x1, y1, TFT_BLACK);
    sprite.drawLine(x1, y1, x2, y2, TFT_BLACK);
    sprite.drawLine(x2, y2, x3, y3, TFT_BLACK);
    sprite.drawLine(x3, y3, x0, y0, TFT_BLACK);
  }

  sprite.pushSprite(0, 0);

  angleX += 0.03f;
  angleY += 0.02f;
  angleZ += 0.01f;

  delay(20);
}
