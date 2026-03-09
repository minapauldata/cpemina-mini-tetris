/*
 * DRONE PROTON TETRIS
 * -------------------
 * Pin 11 → Left       (tap + hold to slide)
 * Pin  9 → Right      (tap + hold to slide)
 * Pin 10 → Speed      (hold for soft-drop)
 * Pin 12 → Short press = Rotate | Long press = Hold piece
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/TomThumb.h>
#include <avr/pgmspace.h>

// =============================================================
//  CONFIGURATION
// =============================================================

// Pins
#define PIN_LEFT      11
#define PIN_RIGHT      9
#define PIN_SPEED     10
#define PIN_ACTION    12   // rotate (short) / hold (long)
#define PIN_SPEAKER    3

// Display
#define SCREEN_W     128
#define SCREEN_H      64
#define DISP_W        64   // logical width  after rotation
#define DISP_H       128   // logical height after rotation

// Game layout
#define HEADER_H      16   // pixels reserved for the top HUD
#define GRID_X         3   // left margin of play field
#define GRID_Y        20   // top  margin of play field (below header)
#define CELL           5   // pixel size of one cell
#define COLS          10
#define ROWS          18

// Piece types
#define TYPES          6
#define NO_HOLD      255   // sentinel — hold slot empty

// Timing (ms)
#define LONG_PRESS   500   // short/long press threshold
#define SLIDE_DELAY  200   // ms before left/right auto-repeat starts
#define SLIDE_REPEAT  80   // ms between repeated slides

// Sound (Hz)
#define SND_CLICK   1047
#define SND_ERASE   2093
#define SND_HOLD     523

// =============================================================
//  PIECE DATA  (stored in flash)
// =============================================================

const char P_SL[2][2][4] PROGMEM = {   // S-left
  {{0,0,1,1},{0,1,1,2}}, {{0,1,1,2},{1,1,0,0}}
};
const char P_SR[2][2][4] PROGMEM = {   // S-right
  {{1,1,0,0},{0,1,1,2}}, {{0,1,1,2},{0,0,1,1}}
};
const char P_L[4][2][4] PROGMEM = {    // L
  {{0,0,0,1},{0,1,2,2}}, {{0,1,2,2},{1,1,1,0}},
  {{0,1,1,1},{0,0,1,2}}, {{0,0,1,2},{1,0,0,0}}
};
const char P_SQ[1][2][4] PROGMEM = {   // Square
  {{0,1,0,1},{0,0,1,1}}
};
const char P_T[4][2][4] PROGMEM = {    // T
  {{0,0,1,0},{0,1,1,2}}, {{0,1,1,2},{1,0,1,1}},
  {{1,0,1,1},{0,1,1,2}}, {{0,1,1,2},{0,0,1,0}}
};
const char P_I[2][2][4] PROGMEM = {    // I
  {{0,1,2,3},{0,0,0,0}}, {{0,0,0,0},{0,1,2,3}}
};

// =============================================================
//  GLOBALS
// =============================================================

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

static const unsigned char PROGMEM logo[] = {
  0x00,0x00,0x18,0x06,0x01,0xc0,0x00,0x00,0xff,0xff,0xff,0xff,
  0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
};

// Grid — bit-packed: grid[col] bit y = row y  (saves ~140 bytes vs bool[10][18])
uint32_t grid[COLS];

// Active piece
uint8_t  currentType, nextType;
uint8_t  rotation;
int8_t   pieceX, pieceY;
int8_t   piece[2][4];

// Hold piece
uint8_t  heldType = NO_HOLD;
bool     holdUsed = false;

// Game state
int      score    = 0;
int      dropRate = 400;   // ms per gravity tick (20 = fast drop)
long     gravTimer, rotDelayer;
bool     gameOver = false;

// Left/right slide state
bool     leftPressed  = false;
bool     rightPressed = false;
long     leftTimer    = 0;
long     rightTimer   = 0;

// Rotate/hold button state machine
bool     chgDown      = false;
bool     chgDone      = false;
bool     chgReleased  = true;
long     chgTimer     = 0;

// =============================================================
//  GRID HELPERS
// =============================================================

inline bool  cellGet(uint8_t x, uint8_t y)           { return (grid[x] >> y) & 1; }
inline void  cellSet(uint8_t x, uint8_t y, bool v)   { v ? grid[x] |= (1UL<<y) : grid[x] &= ~(1UL<<y); }
inline void  gridClear()                              { memset(grid, 0, sizeof(grid)); }

// =============================================================
//  PIECE HELPERS
// =============================================================

// Returns the natural spawn rotation for a piece type on a 90° rotated display.
// Square is symmetric; all others look correct at rotation 1.
// Rotation that makes each piece appear in its natural flat orientation
// on a 90°-rotated display.
//   type 3 (Square) → symmetric, rotation 0
//   type 5 (I-piece) → horizontal at rotation 0
//   all others       → flat at rotation 1
inline uint8_t spawnRotation(uint8_t type) {
  if (type == 3 || type == 5) return 0;
  return 1;
}

// Returns how many rotations a piece has
uint8_t maxRotations(uint8_t type) {
  if (type == 1 || type == 2 || type == 5) return 2;
  if (type == 0 || type == 4)              return 4;
  return 1;
}

// Reads piece coordinates from PROGMEM into a 2×4 buffer
void loadPiece(int8_t dst[2][4], uint8_t type, uint8_t rot) {
  for (uint8_t i = 0; i < 4; i++) {
    switch (type) {
      case 0: dst[0][i]=pgm_read_byte(&P_L [rot][0][i]); dst[1][i]=pgm_read_byte(&P_L [rot][1][i]); break;
      case 1: dst[0][i]=pgm_read_byte(&P_SL[rot][0][i]); dst[1][i]=pgm_read_byte(&P_SL[rot][1][i]); break;
      case 2: dst[0][i]=pgm_read_byte(&P_SR[rot][0][i]); dst[1][i]=pgm_read_byte(&P_SR[rot][1][i]); break;
      case 3: dst[0][i]=pgm_read_byte(&P_SQ[0  ][0][i]); dst[1][i]=pgm_read_byte(&P_SQ[0  ][1][i]); break;
      case 4: dst[0][i]=pgm_read_byte(&P_T [rot][0][i]); dst[1][i]=pgm_read_byte(&P_T [rot][1][i]); break;
      case 5: dst[0][i]=pgm_read_byte(&P_I [rot][0][i]); dst[1][i]=pgm_read_byte(&P_I [rot][1][i]); break;
    }
  }
}

// =============================================================
//  COLLISION
// =============================================================

bool collidesH(int8_t p[2][4], int8_t dx) {
  for (uint8_t i = 0; i < 4; i++) {
    int8_t nx = pieceX + p[0][i] + dx;
    if (nx < 0 || nx >= COLS || cellGet(nx, pieceY + p[1][i])) return true;
  }
  return false;
}

bool collidesDown() {
  for (uint8_t i = 0; i < 4; i++) {
    int8_t ny = pieceY + piece[1][i] + 1;
    int8_t nx = pieceX + piece[0][i];
    if (ny >= ROWS || cellGet(nx, ny)) return true;
  }
  return false;
}

bool canRotateTo(uint8_t rot) {
  int8_t tmp[2][4];
  loadPiece(tmp, currentType, rot);
  for (uint8_t i = 0; i < 4; i++) {
    int8_t nx = pieceX + tmp[0][i];
    if (nx < 0 || nx >= COLS || cellGet(nx, pieceY + tmp[1][i])) return false;
  }
  return true;
}

bool spawnCollides() {
  for (uint8_t i = 0; i < 4; i++)
    if (pieceY + piece[1][i] >= 0 && cellGet(pieceX + piece[0][i], pieceY + piece[1][i]))
      return true;
  return false;
}

// =============================================================
//  PIECE SPAWNING
// =============================================================

void spawnPiece(uint8_t type) {
  currentType = type;
  pieceX      = (type == 5) ? 3 : 4;   // I-piece is 4 wide, others are 2-3
  pieceY      = 0;
  rotation    = spawnRotation(type);
  loadPiece(piece, currentType, rotation);
}

void nextPiece() {
  spawnPiece(nextType);
  nextType = random(TYPES);
  holdUsed = false;
}

// =============================================================
//  ACTIONS
// =============================================================

void doRotate() {
  uint8_t next = (rotation + 1) % maxRotations(currentType);
  if (canRotateTo(next)) {
    rotation = next;
    loadPiece(piece, currentType, rotation);
  }
}

void doHold() {
  if (holdUsed) return;
  tone(PIN_SPEAKER, SND_HOLD, 80); delay(80); noTone(PIN_SPEAKER);
  if (heldType == NO_HOLD) {
    heldType = currentType;
    nextPiece();
  } else {
    uint8_t swap = heldType;
    heldType = currentType;
    spawnPiece(swap);
  }
  holdUsed = true;
}

void lockPiece() {
  for (uint8_t i = 0; i < 4; i++)
    cellSet(pieceX + piece[0][i], pieceY + piece[1][i], true);
}

// =============================================================
//  LINE CLEARING
// =============================================================

void clearLine(int8_t row) {
  tone(PIN_SPEAKER, SND_ERASE, 100); delay(100); noTone(PIN_SPEAKER);
  for (int8_t y = row; y > 0; y--)
    for (uint8_t x = 0; x < COLS; x++)
      cellSet(x, y, cellGet(x, y - 1));
  for (uint8_t x = 0; x < COLS; x++) cellSet(x, 0, false);
  display.invertDisplay(true);  delay(50);
  display.invertDisplay(false);
  score += 10;
}

void checkLines() {
  for (int8_t y = ROWS - 1; y >= 0; y--) {
    bool full = true;
    for (uint8_t x = 0; x < COLS; x++) if (!cellGet(x, y)) { full = false; break; }
    if (full) { clearLine(y); y++; }
  }
}

// =============================================================
//  DRAWING
// =============================================================

// Draw a tiny 2px preview of a piece at pixel position (px, py)
void drawMini(uint8_t type, int px, int py) {
  int8_t tmp[2][4];
  loadPiece(tmp, type, spawnRotation(type));
  for (uint8_t i = 0; i < 4; i++)
    display.fillRect(px + 3*tmp[0][i], py + 3*tmp[1][i], 2, 2, WHITE);
}

void drawHUD() {
  // Border and header divider
  display.drawRect(0, 0, DISP_W, DISP_H, WHITE);
  display.drawLine(0, HEADER_H, DISP_W, HEADER_H, WHITE);

  // Section dividers  |Score|Next|Hold|
  display.drawLine(19, 0, 19, HEADER_H, WHITE);
  display.drawLine(41, 0, 41, HEADER_H, WHITE);

  display.setFont(&TomThumb);
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // Score
  char buf[7]; itoa(score, buf, 10);
  display.setCursor(2, 7);
  display.print(buf);

  // Next
  display.setCursor(21, 6); display.print(F("Next"));
  drawMini(nextType, 24, 8);

  // Hold
  display.setCursor(43, 6); display.print(F("Hold"));
  if (heldType == NO_HOLD) { display.setCursor(43, 13); display.print(F("----")); }
  else                       drawMini(heldType, 46, 8);

  display.setFont(NULL);   // restore default font for the play field
}

void drawGrid() {
  for (uint8_t x = 0; x < COLS; x++)
    for (uint8_t y = 0; y < ROWS; y++)
      if (cellGet(x, y))
        display.fillRect(GRID_X + (CELL+1)*x, GRID_Y + (CELL+1)*y, CELL, CELL, WHITE);
}

void drawActivePiece() {
  for (uint8_t i = 0; i < 4; i++)
    display.fillRect(
      GRID_X + (CELL+1)*(pieceX + piece[0][i]),
      GRID_Y + (CELL+1)*(pieceY + piece[1][i]),
      CELL, CELL, WHITE);
}

void redraw() {
  display.clearDisplay();
  drawHUD();
  drawGrid();
  drawActivePiece();
  display.display();
}

// =============================================================
//  GAME OVER SCREEN
// =============================================================

void resetGame() {
  score        = 0;
  dropRate     = 400;
  rotation     = 0;
  gameOver     = false;
  heldType     = NO_HOLD;
  holdUsed     = false;
  leftPressed  = rightPressed = false;
  chgDown      = chgDone = false;
  chgReleased  = true;
  gridClear();
  randomSeed(analogRead(0));
  nextType = random(TYPES);
  nextPiece();
  gravTimer = millis();
}

void showGameOver() {
  // Descending death tone
  for (int f = 800; f >= 200; f -= 100) { tone(PIN_SPEAKER, f, 60); delay(80); }
  noTone(PIN_SPEAKER);

  // Flash screen
  for (uint8_t i = 0; i < 3; i++) {
    display.invertDisplay(true);  delay(200);
    display.invertDisplay(false); delay(200);
  }

  // Game over screen
  display.clearDisplay();
  display.drawRect(0, 0, DISP_W, DISP_H, WHITE);
  display.setFont(&TomThumb);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(14, 25); display.print(F("GAME"));
  display.setCursor(14, 35); display.print(F("OVER"));
  display.setCursor(4,  52); display.print(F("Score:"));  display.print(score);
  display.setCursor(4,  70); display.print(F("Hold ROTATE"));
  display.setCursor(4,  80); display.print(F("to restart"));
  display.setFont(NULL);
  display.display();

  // Wait for long press to restart
  while (true) {
    if (!digitalRead(PIN_ACTION)) {
      long t = millis();
      while (!digitalRead(PIN_ACTION)) delay(10);
      if (millis() - t >= LONG_PRESS) break;
    }
    delay(10);
  }
  delay(200);
  resetGame();
}

// =============================================================
//  SLIDE HELPER  (shared logic for left and right)
// =============================================================

void handleSlide(bool &wasPressed, long &holdTimer, int8_t dir) {
  if (!digitalRead(dir > 0 ? PIN_RIGHT : PIN_LEFT)) {
    long now = millis();
    if (!wasPressed) {
      if (!collidesH(piece, dir)) { pieceX += dir; redraw(); }
      tone(PIN_SPEAKER, SND_CLICK, 30); delay(30); noTone(PIN_SPEAKER);
      holdTimer   = now;
      wasPressed  = true;
    } else if (now - holdTimer > SLIDE_DELAY) {
      if (!collidesH(piece, dir)) { pieceX += dir; redraw(); }
      holdTimer = now - (SLIDE_DELAY - SLIDE_REPEAT);
    }
  } else {
    wasPressed = false;
  }
}

// =============================================================
//  SETUP
// =============================================================

void setup() {
  pinMode(PIN_LEFT,    INPUT_PULLUP);
  pinMode(PIN_RIGHT,   INPUT_PULLUP);
  pinMode(PIN_SPEED,   INPUT_PULLUP);
  pinMode(PIN_ACTION,  INPUT_PULLUP);
  pinMode(PIN_SPEAKER, OUTPUT);
  Serial.begin(9600);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  display.setRotation(1);
  display.clearDisplay();
  display.drawBitmap(3, 23, logo, 64, 82, WHITE);
  display.display();
  delay(2000);

  resetGame();
  redraw();
}

// =============================================================
//  MAIN LOOP
// =============================================================

void loop() {

  if (gameOver) { showGameOver(); return; }

  // Gravity
  if (millis() - gravTimer > dropRate) {
    checkLines();
    redraw();
    if (collidesDown()) {
      lockPiece();
      nextPiece();
      if (spawnCollides()) gameOver = true;
    } else {
      pieceY++;
    }
    gravTimer = millis();
  }

  // Move left / right
  handleSlide(leftPressed,  leftTimer,  -1);
  handleSlide(rightPressed, rightTimer, +1);

  // Soft drop
  dropRate = !digitalRead(PIN_SPEED) ? 20 : 400;

  // Rotate (short press) / Hold (long press)
  bool held = !digitalRead(PIN_ACTION);
  if (held && chgReleased) {
    if (!chgDown) { chgDown = true; chgTimer = millis(); chgDone = false; }
    if (!chgDone && millis() - chgTimer >= LONG_PRESS) {
      doHold(); redraw();
      chgDone = true; chgReleased = false;
    }
  }
  if (!held) {
    if (chgDown && !chgDone) {
      tone(PIN_SPEAKER, SND_CLICK, 30); delay(30); noTone(PIN_SPEAKER);
      doRotate(); redraw();
    }
    chgDown = chgDone = false;
    chgReleased = true;
  }
}
