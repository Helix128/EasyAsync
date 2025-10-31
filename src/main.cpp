#include <Arduino.h>
#include <EasyAsync.h>
#include <WiFi.h>
#include <HTTPClient.h>

Task endlessTask;

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

  TaskConfig taskCfg;
  taskCfg.name = "EndlessTask";
  taskCfg.priority = 1;
  taskCfg.stackSize = 2048;
  taskCfg.timeoutMs = 2000;
  taskCfg.core = 1;

  auto endlessFn = []() {
      Serial.println("Starting endless task...");
      while (true)
      {
        Serial.println("Task loop running...");
        delay(500);
      }
      
  };
  endlessTask = Async::Create(endlessFn, NOCALLBACK, taskCfg);
  endlessTask.run();
}

void loop() {
  Async::update();
  Serial.println("Main loop running...");
  delay(50);
}
