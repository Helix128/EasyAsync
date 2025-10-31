#include <Arduino.h>
#include <EasyAsync.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <U8g2lib.h>

Task drawTask;
Task updateTask;

void Draw();
void Update();

void setup() {
  Serial.begin(115200);

  while(!Serial){
    Serial.print(".");
    delay(100); 
  }

  Serial.println("OK!");

  AsyncConfig config;
  config.defaultStackSize = 8192;
  config.defaultPriority = 2;
  config.defaultCore = 0;
  config.maxConcurrentTasks = 5;
  config.defaultCore = 0;
  config.executeCallbacksInLoop = false;
  Async::setConfig(config);

  TaskConfig updateCfg;
  updateCfg.name = "UpdateTask";
  updateCfg.core = 1;
  updateTask = Async::Run([](){Update();},NOCALLBACK, updateCfg);

  TaskConfig drawCfg;
  drawCfg.name = "DrawTask";
  drawCfg.core = 0;
  drawTask = Async::Run([](){Draw();},NOCALLBACK, drawCfg);

}

typedef struct Player{
  float y = 0;
  float yVel = 0;
}Player;

Player player;

typedef struct Obstacle{
  float x = 0;
  int y = 0;
  int gap = 30;
}Obstacle;

const int MAX_OBSTACLES = 4;
Obstacle obstacles[MAX_OBSTACLES];

void Start(){
  player.y = 32;
  player.yVel = 0;
  for(int i=0;i<MAX_OBSTACLES;i++){
    obstacles[i].x = 128 + i * 32;
    obstacles[i].y = random(10, 30);
    obstacles[i].gap = random(30, 40);
  }
}
int value = 0;

void Update(){
  Start();
  while(true){
    player.yVel += 0.15;
    player.y += player.yVel;
    if(player.y < 0){
      player.y = 0;
      player.yVel = 0;
    }
    if(player.y > 54){
      player.y = 54;
      player.yVel = 0;
    }
    if(Serial.available()){
      char c = Serial.read();
      if(c == ' '){
        player.yVel = -1.1;
      }
    }

    for(int i=0;i<MAX_OBSTACLES;i++){
      obstacles[i].x -= 1.0;
      if(obstacles[i].x < -10){
        obstacles[i].x += 128;
        obstacles[i].y = random(10, 30);
        obstacles[i].gap = random(30, 40);
        value++;
      }
    }
    delay(10);
  }
}

U8G2_SSD1309_128X64_NONAME2_F_HW_I2C u8g2(U8G2_R0);
void Draw(){
  u8g2.begin();
  u8g2.setBusClock(300000); 
  
  while(true){
    u8g2.clearBuffer();

    u8g2.drawCircle(20, (int)player.y, 4);
    for(int i=0;i<MAX_OBSTACLES;i++){
      u8g2.drawFrame((int)obstacles[i].x, 0, 10, obstacles[i].y);
      u8g2.drawFrame((int)obstacles[i].x, obstacles[i].y + obstacles[i].gap, 10, 64 - (obstacles[i].y + obstacles[i].gap));
    }
    u8g2.sendBuffer();
    delay(1);
  }
}


void loop() {
  Async::update();
}
