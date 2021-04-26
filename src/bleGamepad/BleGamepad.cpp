// -----------------------------------------------------------------------------------
// BLE Gamepad support
// by Drew

#include <Arduino.h>
#include "../../Constants.h"
#include "../../Config.h"
#include "../../ConfigX.h"

#if BLE_GAMEPAD == ON 
  #include "../HAL/HAL.h"
  #include "../debug/Debug.h"
  #include "../pinmaps/Models.h"
  #include <BLEDevice.h>
  #include "../status/MountStatus.h"

  // ===== GamePad Button Assignments =====
  #define LOW_TRIGGER       ":F-#"
  #define UPR_TRIGGER       ":F+#"
  #define BUTTON_A          0x01
  #define BUTTON_B          ":Mp#"  // Spiral search
  #define BUTTON_C          ":R4#"  // rate 4 = 4x
  #define BUTTON_D          ":R8#"  // rate 8 = 48x
  #define FOCUS_LOW         ":FS#"
  #define FOCUS_HIGH        ":FF#"
  #define FOCUS_STOP        ":FQ#"
  #define PARK              ":hP#"
  #define UNPARK            ":hR#"
  #define TRACK_ON          ":Te#"   // Tracking enable
  #define TRACK_OFF         ":Td#"   // Tracking disable
  #define STOP_ALL          ":Q#"
  #define BEEP              ":SX97,0#"
  #define GOTO_CURRENT      ":MS#"

  #define TaskStackSize     4096

  //------ GamePad Definitions -----
  enum
  {
    VB_TRIGGERS = 0,
    VB_JOYX,
    VB_JOYY,
    VB_BTNAB,
    VB_BTNCD,
    VB_NUMBYTES
  };

  //------ GuideRate up/down variables -----
    uint8_t activeGuideRate = 5;
  //  uint8_t featureKeyMode = 1; // guide rate

  // ===== GamePad Button Masks =====

  #define VB_BUTTON_UP      0x00
  #define VB_LOW_TRIGGER    0x01
  #define VB_UPR_TRIGGER    0x02
  #define VB_BUTTON_A       0x01
  #define VB_BUTTON_B       0x02
  #define VB_BUTTON_C       0x01
  #define VB_BUTTON_D       0x02
  #define VB_BUTTON_M       0x00
  #define FRESHFLAG         0x80

  #define JOYTIMEOUT         150   // joystick no activity timeout in mS
  #define JoyStickDeadZone     0   // increase if motors not stopping when in center position
  #define SCANTIMER        15000   // number of ms before rescanning
  #define PARKTIMER         1000   // Accidental push delay for Parking On/Off
  #define FOCUSTIMER        5000   // focus speedup timer
  #define TRACKINGTIMER     1000   // Accidental push delay for Tracking On/Off

  // we will connect to server by MAC address not name
  static BLEAddress *Server_BLE_Address; 
  String My_BLE_Address; 
  String My_BLE_Address1;
  String Scaned_BLE_Address;

  // All four modes send data. However each mode uses different byte positions and
  // values for each of the switches. The joystick acts like a 'D' pad when not in
  // Mouse Mode (no analog value).
  
  typedef void (*NotifyCallback)(BLERemoteCharacteristic*, uint8_t*, size_t, bool);

  // this is the service UUID of the mouse/joystick device (HID)
  static BLEUUID serviceUUID("00001812-0000-1000-8000-00805f9b34fb");

  // Battery Service UUID - not used for now
  //static BLEUUID BatteryServiceUUID("0000180F-0000-1000-8000-00805f9b34fb");

  // this characteristic UUID works for joystick & triggers (report)
  static BLEUUID ReportCharUUID("00002A4D-0000-1000-8000-00805f9b34fb"); // report

  static bool movingNorth = false;
  static bool movingSouth = false;
  static bool movingWest = false;
  static bool movingEast = false;
  static bool triggerPress = false;
  static bool timerReturn = false;
  static bool timerReturn2 = false;  //flag for increased speed
  static bool pressedOnce = false;
  static bool FocusSpd = false; //Focus speed toggle flag
  static bool SpiralInProgess = false; //Spiral search start/stop flag
  static bool dblClk = false; //Double click "clicked once" flag

  static bool doConnect = false;
  static bool Connected = false;
  static bool doScan = false;
  static bool havedevice = false;
  static bool ABpressedOnce = false;
  static bool CDpressedOnce = false;
  static bool isParked = true;
  static bool Tracking = false;
  static bool Parkbtn = false;

  static BLERemoteCharacteristic* pRemoteCharacteristic;
  static BLEAdvertisedDevice* myDevice;

  static BLERemoteCharacteristic* pBatRemoteCharacteristic;

  // pointer to a list of characteristics of the active service,
  // sorted by characteristic UUID
  std::map<std::string, BLERemoteCharacteristic*> *pmap;
  std::map<std::string, BLERemoteCharacteristic*> :: iterator itr;

  // pointer to a list of characteristics of the active service,
  // sorted by characteristic handle
  std::map<std::uint16_t, BLERemoteCharacteristic*> *pmapbh;
  std::map<std::uint16_t, BLERemoteCharacteristic*> :: iterator itrbh;

  // storage for pointers to characteristics we want to work with
  // to do: change to linked list ?
  BLERemoteCharacteristic *bleRcs[4];

  // This is where we store the data from the buttons and joystick
  volatile byte   VrBoxData[VB_NUMBYTES];
  volatile bool   flag = false;         // indicates new data to process

  // joyTimer is a JOYTIMEOUT millisecond re-triggerable timer that sets the joystick 
  // back to center if no activity on the joystick or trigger buttons. 
  volatile uint32_t joyTimer = millis();

  // pushTimer is a millisecond re-triggerable timer that sets the amount
  // of time a key must be pressed before a button state change is triggered
  volatile uint32_t pushTimer = millis();

  // longTimer is a millisecond re-triggerable timer that sets the amount
  // of time a key must be pressed before a button push event is triggered
  volatile uint32_t longTimer = millis();

  // scanTimer is a TimerIncrease millisecond re-triggerable timer that sets the amount
  // of time between rescans on device disconnect
  volatile uint32_t scanTimer = millis();

  // task handles  
  TaskHandle_t HandleJS = NULL;   // handle of the joystick task
  TaskHandle_t HandleAB = NULL;   // handle of the A/B button task
  TaskHandle_t HandleCD = NULL;   // handle of the C/D button task

  // character buffer
  char bfr[80];

  //******************************************************************************
  // HID notification callback handler.
  //******************************************************************************
  static void notifyCallback(
    BLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData,
    size_t length,
    bool isNotify)
    {
      // we are getting the two trigger buttons in the first byte, joyX & joyY in 2nd & 3rd bytes
      // A four byte report is the joystick/trigger buttons.
      // A two byte report is either the A/B buttons or the C/D buttons
      // Low nibble equal to 0x50 indicates A/B buttons.
      if (4 == length)
      {
        // copy data to VrBoxData
        for (int i = VB_TRIGGERS; i < VB_BTNAB; i++)
          VrBoxData[i] = pData[i];
    
        // wake up the joystick/trigger buttons handler task
        if (HandleJS)
          vTaskResume(HandleJS);
          
        // restart the timers
        joyTimer = millis() + JOYTIMEOUT;
        pushTimer = millis() + FOCUSTIMER;
      }
      else if (2 == length)
      {
        // show the received data
        if (0x50 == pData[1])
        {
          // A/B button report, wake the A/B button handler task
          VrBoxData[VB_BTNAB] = pData[0];
          if (HandleAB)
            vTaskResume(HandleAB);

          // restart the parking push delay timer
          longTimer = millis() + PARKTIMER;          
        }
        else
        {
          // C/D button report, wake the C/D button handler task
          // VLF("Wake the C/D button handler task");
          VrBoxData[VB_BTNCD] = pData[0];
          if (HandleCD)
            vTaskResume(HandleCD);
        }
    }
  }
  //  End of notifyCallback

  //******************************************************************************
  // Connection state change event callback handler.
  //******************************************************************************
  class MyClientCallback : public BLEClientCallbacks
  {
    void onConnect(BLEClient* pclient)
    {
      scanTimer = 0;                                          // reinitalize the scan timer
      digitalWrite(LED_STATUS_PIN, LED_STATUS_ON_STATE);      // indicate connected
      VLF("WEM: BLE GamePad Connected");
      mountStatus.update(false);
    }

    void onDisconnect(BLEClient* pclient)
    {
      havedevice = false;
      Connected = false;
      digitalWrite(LED_STATUS_PIN, LED_STATUS_OFF_STATE);     // indicate disconnected
      VLF("WEM: BLE GamePad Disconnected");
      scanTimer = millis() + SCANTIMER;                       // restart the scan timer
    }
  };

  //******************************************************************************
  // Connect to a service, register for notifications from Report Characteristics.
  //******************************************************************************
  bool setupCharacteristics(BLERemoteService* pRemoteService, NotifyCallback pNotifyCallback)
  {
    // get all the characteristics of the service using the handle as the key
    pmapbh = pRemoteService->getCharacteristicsByHandle();
    
    // only interested in report characteristics that have the notify capability
    for (itrbh = pmapbh->begin(); itrbh != pmapbh->end(); itrbh++)
    {
      BLEUUID x = itrbh->second->getUUID();
      // the uuid must match the report uuid
      if (ReportCharUUID.equals(itrbh->second->getUUID()))
      {
        // found a report characteristic
        if (itrbh->second->canNotify())
        {
          // register for notifications from this characteristic
          itrbh->second->registerForNotify(pNotifyCallback);
        }
      }
    }
    return true;
  }
  
  //******************************************************************************
  // Validate the server has the correct name and services we are looking for.
  // The server must have the HID service.
  //******************************************************************************
  bool connectToServer()
  {
    BLEClient*  pClient  = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
    // Connect to the remote BLE Server.
    pClient->connect(myDevice);
    
    // BLE servers may offer several services, each with unique characteristics
    // we can identify the type of service by using the service UUID
    // Obtain a reference to the service we are after in the remote BLE server.
    // this will return a pointer to the remote service if it has a matching service UUID
    
    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == nullptr)
    {
      pClient->disconnect();
      return false;
    }
    setupCharacteristics(pRemoteService, notifyCallback);

    doConnect = false;
    Connected = true;
    return true; 
  }

  //******************************************************************************
  // Scan for BLE servers and find the first one that advertises the service we are looking for.
  //******************************************************************************
  class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks
  {
    // Called for each advertising BLE server.
    void onResult(BLEAdvertisedDevice advertisedDevice)
    {
      // we have found a server, see if it has the name we are looking for
      if (!havedevice) 
      {
        if (advertisedDevice.haveName())
        {
          Server_BLE_Address = new BLEAddress(advertisedDevice.getAddress()); 
          Scaned_BLE_Address = Server_BLE_Address->toString().c_str();
          if ((Scaned_BLE_Address == (My_BLE_Address)) || (Scaned_BLE_Address == (My_BLE_Address1)))
          {
            // we found a server with the correct Mac, see if it has the service we are
            // interested in (HID)
            if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID))
            {
              BLEDevice::getScan()->stop();
              myDevice = new BLEAdvertisedDevice(advertisedDevice);
              doConnect = true;
              doScan = true;
              havedevice = true;
              scanTimer = 0; // reinitalize the scan timer
            }           
          } 
        }
      }
    }
  };

  // All of these tasks are designed to run forever. The tasks are resumed when
  // a notification message is received with new data.
  //******************************************************************************
  // Joystick handler Task.
  // Moving the joystick off center causes this task to be resumed about every
  // 15ms. Press of either trigger button will also resume this task.
  //******************************************************************************
  void taskJoyStick(void *parameter)
  {
    int8_t  x;
    int8_t  y;
    uint8_t triggers;

    // forever loop
    while(true)
    {
      // give up the CPU, wait for new data
      vTaskSuspend(NULL);

      // we just woke up, new data is available, convert joystick data to
      // signed 8 bit integers
      x = (int8_t)VrBoxData[VB_JOYX];
      y = (int8_t)VrBoxData[VB_JOYY];
      triggers = VrBoxData[VB_TRIGGERS];

      if (pressedOnce)
      {
        pressedOnce = false;
        continue;
      }

      if (triggers & VB_LOW_TRIGGER)
      {
        // the lower trigger button is pressed
        if (FocusSpd)
        {
          Ser.print(FOCUS_HIGH); 
          FocusSpd = false;
        }
        else Ser.print(FOCUS_LOW); 
          
        Ser.print(LOW_TRIGGER);         
        triggerPress = true;
        pressedOnce = true;
        continue;
      }
        
      if (triggers & VB_UPR_TRIGGER)
      {
        // the upper trigger button is pressed
        if (FocusSpd)
          {
            Ser.print(FOCUS_HIGH); 
            FocusSpd = false;
          }
        else Ser.print(FOCUS_LOW);  
      
        Ser.print(UPR_TRIGGER);         
        triggerPress = true;
        pressedOnce = true;
        continue;
      }
        
      if (y < -JoyStickDeadZone)
      {
        // move North
        if (!movingNorth) 
        {
          movingNorth = true;
          Ser.print(":Mn#");
        }
      }
      else if (y > JoyStickDeadZone)
      {
        // move South
        if (!movingSouth) 
        {
          movingSouth = true;
          Ser.print(":Ms#");
        }
      }
      if (x < -JoyStickDeadZone)
      {
        // move East
        if (!movingEast) 
        {
          movingEast = true;
          Ser.print(":Me#");
        }
      }
      else if (x > JoyStickDeadZone)
      {
        // move West
        if (!movingWest) 
        {
          movingWest = true;
          Ser.print(":Mw#");
        }
      }
    
      if (triggerPress && (VrBoxData[VB_TRIGGERS] == 0))
      {
        pushTimer = 0;
        triggerPress = false;   
        FocusSpd = false;
        Ser.print(FOCUS_STOP);          
        continue;          
      }
              
      // joystick has been centered for JOYTIMEOUT ms 
      if (timerReturn) 
      {
        if ((JoyStickDeadZone == 0) && (movingNorth || movingSouth || movingEast || movingWest))
        {
          movingNorth = false; 
          movingSouth = false; 
          movingEast = false; 
          movingWest = false;                                  
          timerReturn = false;
          Ser.print(STOP_ALL);          
        }        
      }
    }
  } 
  // End of taskJoyStick

  //******************************************************************************
  // A & B Buttons handler Task.
  // Press of either the A or B button will resume this task. 
  //******************************************************************************
  void taskButtonAB(void *parameter)
  {
    uint8_t buttons;
  
    while(true)
    {
      // give up the CPU, wait for new data
      vTaskSuspend(NULL);
      
      // we just woke up, new data is available
      buttons = VrBoxData[VB_BTNAB];

      if (buttons & VB_BUTTON_A)
      {
        Ser.print(GOTO_CURRENT);
        Ser.print(BEEP);    
      }

      if (buttons & VB_BUTTON_B)
      {
        // button B pressed
        // Spiral search
        if (SpiralInProgess == false)
          {
            SpiralInProgess = true;
            Ser.print(BUTTON_B);
          }
        else
          {
            SpiralInProgess = false; 
            Ser.print(BUTTON_B);
            delay(100);
            Ser.print(STOP_ALL);
          }
      }
    }
  } 
  //  End of taskButtonAB

  //******************************************************************************
  // C & D Buttons handler Task. Also M button
  // Press of either the C or D button will resume this task. 
  //******************************************************************************
  void taskButtonCD(void *parameter)
  {
    uint8_t buttons;
    
    while(true)
    {
      // give up the CPU
      vTaskSuspend(NULL);

      // we just woke up, new data is available
      buttons = VrBoxData[VB_BTNCD];

      if (CDpressedOnce)
      {
        CDpressedOnce = false;        
        continue;
      }
      
      if (VB_BUTTON_M == buttons)
      {
          mountStatus.update(false);

      // emergency stop
      if (mountStatus.slewing())
      {   
        Ser.print(STOP_ALL); 
        continue;
      }
      
      else if (mountStatus.atHome() && !mountStatus.tracking())
      {
        Ser.print(TRACK_ON);
      }        
      else if (mountStatus.parked())
      {
        Ser.print(UNPARK);         
      }
      else if (!mountStatus.parked())
      {
        Ser.print(PARK);
        Ser.print(BEEP);               
      }
    }

      if (buttons & VB_BUTTON_C)
      {
        // button C pressed
        CDpressedOnce = true; 
        (activeGuideRate--);
        if (activeGuideRate<4)  activeGuideRate=4;
        if (activeGuideRate>10) activeGuideRate=10;
        char cmd[5]= ":Rn#"; cmd[2] = '0' + activeGuideRate - 1;
        Ser.print(cmd);
      }

      if (buttons & VB_BUTTON_D)
      {
        // button D pressed
        CDpressedOnce = true; 
        (activeGuideRate++);
        if (activeGuideRate<4)  activeGuideRate=4;
        if (activeGuideRate>10) activeGuideRate=10;
        char cmd[5]= ":Rn#"; cmd[2] = '0' + activeGuideRate - 1;
        Ser.print(cmd);
      }
    } 
  }
  //  End of taskButtonCD

  //******************************************************************************
  //******************************************************************************

  void bleInit()
  {
    // enable BT ble only (no classic)
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    // free memory used by BT Classic to prevent out of memory crash
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  }

  void DoScan() 
  {
    if (scanTimer && (scanTimer < millis()))
    {
        // Retrieve a GATT Scanner and set the callback we want to use to be informed 
        // when we have detected a new device.  Specify that we want active scanning
        // and start the scan.
      
        BLEScan* pBLEScan = BLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
        pBLEScan->setInterval(1349);
        pBLEScan->setWindow(499);
        pBLEScan->setActiveScan(true);
        pBLEScan->start(1); // scan 1 second

      // restart the scan timer
      scanTimer = millis() + SCANTIMER;
      VLF("WEM: Scanning for BLE GamePad");
    }
  }
  // End of DoScan.

  //******************************************************************************
  // This is the first and after disconnect server connection
  //******************************************************************************
  void bleConnTest() 
  {
    // If the flag "doConnect" is true then we have scanned for and found the desired
    // BLE Server with which we wish to connect.  Now we connect to it.  Once we are 
    // connected we set the connected flag to be true.
    
    if (doConnect == true) 
    {
      connectToServer();
    }
    else
    {
      doConnect = false;
      doScan = true;
      (DoScan());
    }
  }
  // End of bleConnTest

  void bleSetup()
  {
    My_BLE_Address = BLE_GP_ADDR;
    My_BLE_Address1 = BLE_GP_ADDR1;  

    BaseType_t xReturned;

    // create tasks to handle the joystick and buttons
    xReturned = xTaskCreate(taskJoyStick,             // task to handle activity on the joystick.
                            "Joystick",               // String with name of task.
                            TaskStackSize,            // Stack size in 32 bit words.
                            NULL,                     // Parameter passed as input of the task
                            1,                        // Priority of the task.
                            &HandleJS);               // Task handle.
  
    xReturned = xTaskCreate(taskButtonAB,             // task to handle activity on the A & B buttons.
                            "ButtonsAB",              // String with name of task.
                            TaskStackSize,            // Stack size in 32 bit words.
                            NULL,                     // Parameter passed as input of the task
                            1,                        // Priority of the task.
                            &HandleAB);               // Task handle.

    xReturned = xTaskCreate(taskButtonCD,             // task to handle activity on the C & D buttons.
                            "ButtonsCD",              // String with name of task.
                            TaskStackSize,            // Stack size in 32 bit words.
                            NULL,                     // Parameter passed as input of the task
                            1,                        // Priority of the task.
                            &HandleCD);               // Task handle.

    BLEDevice::init("");

    if (!Connected)
    {
      // Retrieve a GATT Scanner and set the callback we want to use to be informed 
      // when we have detected a new device.  Specify that we want active scanning
      // and start the scan.
      
      BLEScan* pBLEScan = BLEDevice::getScan();
      pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
      pBLEScan->setInterval(1349);
      pBLEScan->setWindow(150);  
      pBLEScan->setActiveScan(true);
      pBLEScan->start(2, false); // scan
        
      delay(3000); // delay for scan
    }
    
    if (!Connected)
    {
      doScan = true;
      // restart the scan timer
      scanTimer = millis() + SCANTIMER;
    }
      
    bleConnTest();

    VLF("WEM: Starting BLE GamePad"); 
  }
  // End of bleSetup.

  void bleTimers() 
  {
    // joystick no activity detector
      
    if (Connected)
    {
      if (joyTimer && (joyTimer < millis()))
      {
        // no joystick notification for JOYTIMEOUT mS, center the joystick
        VrBoxData[VB_JOYX] = VrBoxData[VB_JOYY] = 0;
        // wake up the joystick task
        timerReturn = true;
        vTaskResume(HandleJS);
        joyTimer = 0;
      }
            
      if (pushTimer && (pushTimer < millis()))
      {
        // Focus button held down for FOCUSTIMER, speed up
        FocusSpd = true;
        // wake up the joystick task
        timerReturn = true;
        vTaskResume(HandleJS);
        pushTimer = 0;          
      }
      else FocusSpd = false;
    }
  }
  // end of bleTimers

#endif 
