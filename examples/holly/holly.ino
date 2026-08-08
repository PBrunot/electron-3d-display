#include <TFT_eSPI.h>
#include "images.h"

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);

void setup() {
    pinMode(20,OUTPUT);
    digitalWrite(20L,1);

  tft.begin();          
  tft.invertDisplay(1);  
  tft.setRotation(4);
  tft.fillScreen(TFT_BLACK);
 // tft.setSwapBytes(true);
 
    sprite.createSprite(240,240);
    sprite.setSwapBytes(true);
    sprite.setTextDatum(4);
}

int n=-15;
void loop()
{
    n++;
    if(n>=0 && n<79)
    sprite.pushImage(64,40,116,158,logo2[n]);
    else
    sprite.fillRect(64,40,116,158,TFT_BLACK);

    if(n>86) n=-15;
    delay(28);

    sprite.pushSprite(0,0);
}