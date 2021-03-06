/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Main loop
 * Feb 2015 @ OpenSprinkler.com
 *
 * This file is part of the OpenSprinkler Firmware
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <limits.h>
#include "OpenSprinkler.h"
#include "program.h"
#include "weather.h"
#include "server.h"
#include <FS.h>
#include "espconnect.h"

char ether_buffer[ETHER_BUFFER_SIZE];
unsigned long getNtpTime();
void reset_all_stations();
void reset_all_stations_immediate();
void push_message(byte type, uint32_t lval=0, float fval=0.f, const char* sval=NULL);
void manual_start_program(byte, byte);
void httpget_callback(byte, uint16_t, uint16_t);


// Small variations have been added to the timing values below
// to minimize conflicting events
#define NTP_SYNC_INTERVAL       86403L  // NYP sync interval, 24 hrs
#define RTC_SYNC_INTERVAL       60      // RTC sync interval, 60 secs
#define CHECK_NETWORK_INTERVAL  601     // Network checking timeout, 10 minutes
#define CHECK_WEATHER_TIMEOUT   7201    // Weather check interval: 2 hours
#define CHECK_WEATHER_SUCCESS_TIMEOUT 86433L // Weather check success interval: 24 hrs
#define LCD_BACKLIGHT_TIMEOUT   15      // LCD backlight timeout: 15 secs
#define PING_TIMEOUT            200     // Ping test timeout: 200 ms

extern char tmp_buffer[];       // scratch buffer

ESP8266WebServer *wifi_server = NULL;
static uint16_t led_blink_ms = LED_FAST_BLINK;

// ====== Object defines ======
OpenSprinkler os; // OpenSprinkler object
ProgramData pd;   // ProgramdData object

/* ====== Robert Hillman (RAH)'s implementation of flow sensor ======
 * flow_begin - time when valve turns on
 * flow_start - time when flow starts being measured (i.e. 2 mins after flow_begin approx
 * flow_stop - time when valve turns off (last rising edge pulse detected before off)
 * flow_gallons - total # of gallons+1 from flow_start to flow_stop
 * flow_last_gpm - last flow rate measured (averaged over flow_gallons) from last valve stopped (used to write to log file). */
ulong flow_begin, flow_start, flow_stop, flow_gallons;
ulong flow_count = 0;
float flow_last_gpm=0;
byte prev_flow_state = HIGH;

void flow_poll() {
  byte curr_flow_state = digitalRead(PIN_FLOWSENSOR);
  if(os.options[OPTION_SENSOR2_TYPE]!=SENSOR_TYPE_FLOW) return;

  if(!(prev_flow_state==HIGH && curr_flow_state==LOW)) {
    prev_flow_state = curr_flow_state;
    return;
  }
  prev_flow_state = curr_flow_state;

  ulong curr = millis();

  if(curr <= os.flowcount_time_ms) return;  // debounce threshold: 1ms
  flow_count++;
  os.flowcount_time_ms = curr;

  /* RAH implementation of flow sensor */
  if (flow_start==0) { flow_gallons=0; flow_start=curr;}  // if first pulse, record time
  if ((curr-flow_start)<90000) { flow_gallons=0; } // wait 90 seconds before recording flow_begin
  else {  if (flow_gallons==1)  {  flow_begin = curr;}}
  flow_stop = curr; // get time in ms for stop
  flow_gallons++;  // increment gallon count for each interrupt
  /* End of RAH implementation of flow sensor */
}

volatile byte flow_isr_flag = false;
/** Flow sensor interrupt service routine */
ICACHE_RAM_ATTR void flow_isr() // for ESP8266, ISR must be marked ICACHE_RAM_ATTR
{
  flow_isr_flag = true;
}

// ====== UI defines ======
static char ui_anim_chars[3] = {'.', 'o', 'O'};

#define UI_STATE_DEFAULT   0
#define UI_STATE_DISP_IP   1
#define UI_STATE_DISP_GW   2
#define UI_STATE_RUNPROG   3

static byte ui_state = UI_STATE_DEFAULT;
static byte ui_state_runprog = 0;

bool ui_confirm(PGM_P str) {
  os.lcd_print_line_clear_pgm(str, 0);
  os.lcd_print_line_clear_pgm(PSTR("(B1:No, B3:Yes)"), 1);
  byte button;
  ulong timeout = millis()+4000;
  do {
    button = os.button_read(BUTTON_WAIT_NONE);
    if((button&BUTTON_MASK)==BUTTON_3 && (button&BUTTON_FLAG_DOWN)) return true;
    if((button&BUTTON_MASK)==BUTTON_1 && (button&BUTTON_FLAG_DOWN)) return false;
    delay(10);
  } while(millis() < timeout);
  return false;
}

void ui_state_machine() {
 
  // process screen led
    static ulong led_toggle_timeout = 0;
  if(led_blink_ms) {
    if(millis()>led_toggle_timeout) {
      os.led_toggle();
      led_toggle_timeout = millis() + led_blink_ms;
    }
  }
  else {
    os.led_on();
  }
  
  if (!os.button_timeout) {
    ui_state = UI_STATE_DEFAULT;  // also recover to default state
  }

  // read button, if something is pressed, wait till release
  byte button; //= os.button_read(BUTTON_WAIT_HOLD);

  if (button & BUTTON_FLAG_DOWN) {   // repond only to button down events
    os.button_timeout = LCD_BACKLIGHT_TIMEOUT;
  } else {
    return;
  }

  switch(ui_state) {
  case UI_STATE_DEFAULT:
    switch (button & BUTTON_MASK) {
    case BUTTON_1:
      if (button & BUTTON_FLAG_HOLD) {  // holding B1
        if (digitalRead(PIN_BUTTON_3)==0) { // if B3 is pressed while holding B1, run a short test (internal test)
          if(!ui_confirm(PSTR("Start 2s test?"))) {ui_state = UI_STATE_DEFAULT; break;}
          manual_start_program(255, 0);
        } else if (digitalRead(PIN_BUTTON_2)==0) { // if B2 is pressed while holding B1, display gateway IP
          Serial.println("");
          Serial.print(WiFi.gatewayIP());
          os.lcd_print_pgm(PSTR(" (gwip)"));
          ui_state = UI_STATE_DISP_IP;
        } else {  // if no other button is clicked, stop all zones
          if(!ui_confirm(PSTR("Stop all zones?"))) {ui_state = UI_STATE_DEFAULT; break;}
          reset_all_stations();
        }
      } else {  // clicking B1: display device IP and port
        Serial.println("");
        Serial.print(WiFi.localIP());
        os.lcd_print_pgm(PSTR(":"));
        uint16_t httpport = (uint16_t)(os.options[OPTION_HTTPPORT_1]<<8) + (uint16_t)os.options[OPTION_HTTPPORT_0];
        Serial.print(httpport);
        os.lcd_print_pgm(PSTR(" (ip:port)"));
        ui_state = UI_STATE_DISP_IP;
      }
      break;
    case BUTTON_2:
      if (button & BUTTON_FLAG_HOLD) {  // holding B2
        if (digitalRead(PIN_BUTTON_1)==0) { // if B1 is pressed while holding B2, display external IP
          os.lcd_print_ip((byte*)(&os.nvdata.external_ip), 1);
          os.lcd_print_pgm(PSTR(" (eip)"));
          ui_state = UI_STATE_DISP_IP;
        } else if (digitalRead(PIN_BUTTON_3)==0) {  // if B3 is pressed while holding B2, display last successful weather call
          Serial.println("");
          os.lcd_print_time(os.checkwt_success_lasttime);
          os.lcd_print_pgm(PSTR(" (lswc)"));
          ui_state = UI_STATE_DISP_IP;          
        } else {  // if no other button is clicked, reboot
          if(!ui_confirm(PSTR("Reboot device?"))) {ui_state = UI_STATE_DEFAULT; break;}
          os.reboot_dev();
        }
      } else {  // clicking B2: display MAC
        Serial.println("");
        byte mac[6];
        WiFi.macAddress(mac);
        os.lcd_print_mac(mac);
        os.lcd_print_pgm(PSTR(" (mac)"));
        ui_state = UI_STATE_DISP_GW;
      }
      break;
    case BUTTON_3:
      if (button & BUTTON_FLAG_HOLD) {  // holding B3
        if (digitalRead(PIN_BUTTON_1)==0) {  // if B1 is pressed while holding B3, display up time
          Serial.println("");
          os.lcd_print_time(os.powerup_lasttime);
          os.lcd_print_pgm(PSTR(" (lupt)"));
          ui_state = UI_STATE_DISP_IP;              
        } else if(digitalRead(PIN_BUTTON_2)==0) {  // if B2 is pressed while holding B3, reset to AP and reboot
          if(!ui_confirm(PSTR("Reset to AP?"))) {ui_state = UI_STATE_DEFAULT; break;}
          os.reset_to_ap();
        } else {  // if no other button is clicked, go to Run Program main menu
          os.lcd_print_line_clear_pgm(PSTR("Run a Program:"), 0);
          os.lcd_print_line_clear_pgm(PSTR("Click B3 to list"), 1);
          ui_state = UI_STATE_RUNPROG;
        }
      } else {  // clicking B3: switch board display (cycle through master and all extension boards)
        os.status.display_board = (os.status.display_board + 1) % (os.nboards);
      }
      break;
    }
    break;
  case UI_STATE_DISP_IP:
  case UI_STATE_DISP_GW:
    ui_state = UI_STATE_DEFAULT;
    break;
  case UI_STATE_RUNPROG:
    if ((button & BUTTON_MASK)==BUTTON_3) {
      if (button & BUTTON_FLAG_HOLD) {
        // start
        manual_start_program(ui_state_runprog, 0);
        ui_state = UI_STATE_DEFAULT;
      } else {
        ui_state_runprog = (ui_state_runprog+1) % (pd.nprograms+1);
        os.lcd_print_line_clear_pgm(PSTR("Hold B3 to start"), 0);
        if(ui_state_runprog > 0) {
          ProgramStruct prog;
          pd.read(ui_state_runprog-1, &prog);
          os.lcd_print_line_clear_pgm(PSTR(" "), 1);
          Serial.println("");
          Serial.print((int)ui_state_runprog);
          os.lcd_print_pgm(PSTR(". "));
          Serial.print(prog.name);
        } else {
          os.lcd_print_line_clear_pgm(PSTR("0. Test (1 min)"), 1);
        }
      }
    }
    break;
  }
}

// ======================
// Setup Function
// ======================
void do_setup() {
  /* Clear WDT reset flag. */
  if(wifi_server) { delete wifi_server; wifi_server = NULL; }
  WiFi.persistent(false);
  led_blink_ms = LED_FAST_BLINK;
  DEBUG_BEGIN(115200);
  Serial.println("Initializing...");
  os.begin();          // OpenSprinkler init
  Serial.print("os-begin...");
  os.options_setup();  // Setup options
  Serial.print("os-options_setup...");
  pd.init();            // ProgramData init
  Serial.print("pd-init...");
  setSyncInterval(RTC_SYNC_INTERVAL);  // RTC sync interval
  // if rtc exists, sets it as time sync source
  //setSyncProvider(RTC.get);
  os.lcd_print_time(os.now_tz());  // display time to LCD
  os.powerup_lasttime = os.now_tz();
  
   if (os.start_network()) {  // initialize network
    os.status.network_fails = 0;
  } else {
    os.status.network_fails = 1;
  }
  os.status.req_network = 0;
  os.status.req_ntpsync = 1;

  os.apply_all_station_bits(); // reset station bits

  os.button_timeout = LCD_BACKLIGHT_TIMEOUT;
}

// Arduino software reset function
void(* sysReset) (void) = 0;

void write_log(byte type, ulong curr_time);
void schedule_all_stations(ulong curr_time);
void turn_off_station(byte sid, ulong curr_time);
void process_dynamic_events(ulong curr_time);
void check_network();
void check_weather();
void perform_ntp_sync();
void delete_log(char *name);

void start_server_ap();
void start_server_client();
unsigned long reboot_timer = 0;

/** Main Loop */
void do_loop() {

  /** If flow_isr_flag is on, do flow sensing.
     todo: not the most efficient way, as we can't do I2C inside ISR.
     need to figure out a more efficient way to do flow sensing */
  if(flow_isr_flag) {
    flow_isr_flag = false;
    flow_poll();
  }

  static ulong last_time = 0;
  static ulong last_minute = 0;

  byte bid, sid, s, pid, qid, bitvalue;
  ProgramStruct prog;

  os.status.mas = os.options[OPTION_MASTER_STATION];
  os.status.mas2= os.options[OPTION_MASTER_STATION_2];
  time_t curr_time = os.now_tz();

  // ====== Process Ethernet packets ======
  static ulong connecting_timeout;
  switch(os.state) {
  case OS_STATE_INITIAL:
    if(os.get_wifi_mode()==WIFI_MODE_AP) {
      start_server_ap();
      os.state = OS_STATE_CONNECTED;
      connecting_timeout = 0;
    } else {
      led_blink_ms = LED_SLOW_BLINK;
      start_network_sta(os.wifi_config.ssid.c_str(), os.wifi_config.pass.c_str());
      os.config_ip();
      os.state = OS_STATE_CONNECTING;
      connecting_timeout = millis() + 120000L;
      DEBUG_PRINTLN("");
      DEBUG_PRINT(F("Connecting to..."));      
      DEBUG_PRINT(os.wifi_config.ssid);
    }
    break;
    
  case OS_STATE_TRY_CONNECT:
    led_blink_ms = LED_SLOW_BLINK;  
    start_network_sta_with_ap(os.wifi_config.ssid.c_str(), os.wifi_config.pass.c_str());
    os.config_ip();
    os.state = OS_STATE_CONNECTED;
    break;
   
  case OS_STATE_CONNECTING:
    if(WiFi.status() == WL_CONNECTED) {
      led_blink_ms = 0;
      start_server_client();
      os.state = OS_STATE_CONNECTED;
      DEBUG_PRINT(F("...connected."));
      connecting_timeout = 0;
    } else {
      if(millis()>connecting_timeout) {
        os.state = OS_STATE_INITIAL;
        DEBUG_PRINT(F("...timeout!!!"));
      }
    }
    break;
    
  case OS_STATE_CONNECTED:
    if(os.get_wifi_mode() == WIFI_MODE_AP) {
      wifi_server->handleClient();
      connecting_timeout = 0;
      if(os.get_wifi_mode()==WIFI_MODE_STA) {
        // already in STA mode, waiting to reboot
        break;
      }
      if(WiFi.status()==WL_CONNECTED && WiFi.localIP()) {
        os.wifi_config.mode = WIFI_MODE_STA;
        os.options_save(true);
        os.reboot_dev();
      }
    }
    else {
      if(WiFi.status() == WL_CONNECTED) {
        wifi_server->handleClient();
        connecting_timeout = 0;
      } else {
        os.state = OS_STATE_INITIAL;
      }
    }
    break;
  }
  ui_state_machine();
  // Process Ethernet packets

  // The main control loop runs once every second
  if (curr_time != last_time) {
    last_time = curr_time;
    if (os.button_timeout) os.button_timeout--;
    
    if(reboot_timer && millis() > reboot_timer) {
      os.reboot_dev();
    }
    
    // ====== Check raindelay status ======
    if (os.status.rain_delayed) {
      if (curr_time >= os.nvdata.rd_stop_time) {  // rain delay is over
        os.raindelay_stop();
      }
    } else {
      if (os.nvdata.rd_stop_time > curr_time) {   // rain delay starts now
        os.raindelay_start();
      }
    }

    // ====== Check controller status changes and write log ======
    if (os.old_status.rain_delayed != os.status.rain_delayed) {
      if (os.status.rain_delayed) {
        // rain delay started, record time
        os.raindelay_start_time = curr_time;
        push_message(IFTTT_RAINSENSOR, LOGDATA_RAINDELAY, 1);
      } else {
        // rain delay stopped, write log
        write_log(LOGDATA_RAINDELAY, curr_time);
        push_message(IFTTT_RAINSENSOR, LOGDATA_RAINDELAY, 0);
      }
      os.old_status.rain_delayed = os.status.rain_delayed;
    }

    // ====== Check rain sensor status ======
    if (os.options[OPTION_SENSOR1_TYPE] == SENSOR_TYPE_RAIN) { // if a rain sensor is connected
      os.rainsensor_status();
      if (os.old_status.rain_sensed != os.status.rain_sensed) {
        if (os.status.rain_sensed) {
          // rain sensor on, record time
          os.sensor_lasttime = curr_time;
          push_message(IFTTT_RAINSENSOR, LOGDATA_RAINSENSE, 1);
        } else {
          // rain sensor off, write log
          if (curr_time>os.sensor_lasttime+10) {  // add a 10 second threshold
                                                  // to avoid faulty rain sensors generating
                                                  // too many log records
            write_log(LOGDATA_RAINSENSE, curr_time);
            push_message(IFTTT_RAINSENSOR, LOGDATA_RAINSENSE, 0);
          }
        }
        os.old_status.rain_sensed = os.status.rain_sensed;
      }
    }

    // ===== Check program switch status =====
    if (os.programswitch_status(curr_time)) {
      reset_all_stations_immediate(); // immediately stop all stations
      if(pd.nprograms > 0)  manual_start_program(1, 0);
    }

    // ====== Schedule program data ======
    ulong curr_minute = curr_time / 60;
    boolean match_found = false;
    RuntimeQueueStruct *q;
    // since the granularity of start time is minute
    // we only need to check once every minute
    if (curr_minute != last_minute) {
      last_minute = curr_minute;
      // check through all programs
      for(pid=0; pid<pd.nprograms; pid++) {
        pd.read(pid, &prog);
        if(prog.check_match(curr_time)) {
          // program match found
          // process all selected stations
          for(sid=0;sid<os.nstations;sid++) {
            bid=sid>>3;
            s=sid&0x07;
            // skip if the station is a master station (because master cannot be scheduled independently
            if ((os.status.mas==sid+1) || (os.status.mas2==sid+1))
              continue;

            // if station has non-zero water time and the station is not disabled
            if (prog.durations[sid] && !(os.station_attrib_bits_read(ADDR_NVM_STNDISABLE+bid)&(1<<s))) {
              // water time is scaled by watering percentage
              ulong water_time = water_time_resolve(prog.durations[sid]);
              // if the program is set to use weather scaling
              if (prog.use_weather) {
                byte wl = os.options[OPTION_WATER_PERCENTAGE];
                water_time = water_time * wl / 100;
                if (wl < 20 && water_time < 10) // if water_percentage is less than 20% and water_time is less than 10 seconds
                                                // do not water
                  water_time = 0;
              }

              if (water_time) {
                // check if water time is still valid
                // because it may end up being zero after scaling
                q = pd.enqueue();
                if (q) {
                  q->st = 0;
                  q->dur = water_time;
                  q->sid = sid;
                  q->pid = pid+1;
                  match_found = true;
                } else {
                  // queue is full
                }
              }// if water_time
            }// if prog.durations[sid]
          }// for sid
          if(match_found) push_message(IFTTT_PROGRAM_SCHED, pid, prog.use_weather?os.options[OPTION_WATER_PERCENTAGE]:100);
        }// if check_match
      }// for pid

      // calculate start and end time
      if (match_found) {
        schedule_all_stations(curr_time);

        // For debugging: print out queued elements
        /*DEBUG_PRINT("en:");
        for(q=pd.queue;q<pd.queue+pd.nqueue;q++) {
          DEBUG_PRINT("[");
          DEBUG_PRINT(q->sid);
          DEBUG_PRINT(",");
          DEBUG_PRINT(q->dur);
          DEBUG_PRINT(",");
          DEBUG_PRINT(q->st);
          DEBUG_PRINT("]");
        }
        DEBUG_PRINTLN("");*/
      }
    }//if_check_current_minute

    // ====== Run program data ======
    // Check if a program is running currently
    // If so, do station run-time keeping
    if (os.status.program_busy){
      // first, go through run time queue to assign queue elements to stations
      q = pd.queue;
      qid=0;
      for(;q<pd.queue+pd.nqueue;q++,qid++) {
        sid=q->sid;
        byte sqi=pd.station_qid[sid];
        // skip if station is already assigned a queue element
        // and that queue element has an earlier start time
        if(sqi<255 && pd.queue[sqi].st<q->st) continue;
        // otherwise assign the queue element to station
        pd.station_qid[sid]=qid;
      }
      // next, go through the stations and perform time keeping
      for(bid=0;bid<os.nboards; bid++) {
        bitvalue = os.station_bits[bid];
        for(s=0;s<8;s++) {
          byte sid = bid*8+s;

          // skip master station
          if (os.status.mas == sid+1) continue;
          if (os.status.mas2== sid+1) continue;
          if (pd.station_qid[sid]==255) continue;

          q = pd.queue + pd.station_qid[sid];
          // check if this station is scheduled, either running or waiting to run
          if (q->st > 0) {
            // if so, check if we should turn it off
            if (curr_time >= q->st+q->dur) {
              turn_off_station(sid, curr_time);
            }
          }
          // if current station is not running, check if we should turn it on
          if(!((bitvalue>>s)&1)) {
            if (curr_time >= q->st && curr_time < q->st+q->dur) {

              //turn_on_station(sid);
              os.set_station_bit(sid, 1);

              // RAH implementation of flow sensor
              flow_start=0;

            } //if curr_time > scheduled_start_time
          } // if current station is not running
        }//end_s
      }//end_bid

      // finally, go through the queue again and clear up elements marked for removal
      int qi;
      for(qi=pd.nqueue-1;qi>=0;qi--) {
        q=pd.queue+qi;
        if(!q->dur || curr_time>=q->st+q->dur)  {
          pd.dequeue(qi);
        }
      }

      // process dynamic events
      process_dynamic_events(curr_time);

      // activate / deactivate valves
      os.apply_all_station_bits();

      // check through runtime queue, calculate the last stop time of sequential stations
      pd.last_seq_stop_time = 0;
      ulong sst;
      byte re=os.options[OPTION_REMOTE_EXT_MODE];
      q = pd.queue;
      for(;q<pd.queue+pd.nqueue;q++) {
        sid = q->sid;
        bid = sid>>3;
        s = sid&0x07;
        // check if any sequential station has a valid stop time
        // and the stop time must be larger than curr_time
        sst = q->st + q->dur;
        if (sst>curr_time) {
          // only need to update last_seq_stop_time for sequential stations
          if (os.station_attrib_bits_read(ADDR_NVM_STNSEQ+bid)&(1<<s) && !re) {
            pd.last_seq_stop_time = (sst>pd.last_seq_stop_time ) ? sst : pd.last_seq_stop_time;
          }
        }
      }

      // if the runtime queue is empty
      // reset all stations
      if (!pd.nqueue) {
        // turn off all stations
        os.clear_all_station_bits();
        os.apply_all_station_bits();
        // reset runtime
        pd.reset_runtime();
        // reset program busy bit
        os.status.program_busy = 0;
        // log flow sensor reading if flow sensor is used
        if(os.options[OPTION_SENSOR2_TYPE]==SENSOR_TYPE_FLOW) {
          write_log(LOGDATA_FLOWSENSE, curr_time);
          push_message(IFTTT_FLOWSENSOR, (flow_count>os.flowcount_log_start)?(flow_count-os.flowcount_log_start):0);
        }

        // in case some options have changed while executing the program
        os.status.mas = os.options[OPTION_MASTER_STATION]; // update master station
        os.status.mas2= os.options[OPTION_MASTER_STATION_2]; // update master2 station
      }
    }//if_some_program_is_running

    // handle master
    if (os.status.mas>0) {
      int16_t mas_on_adj = water_time_decode_signed(os.options[OPTION_MASTER_ON_ADJ]);
      int16_t mas_off_adj= water_time_decode_signed(os.options[OPTION_MASTER_OFF_ADJ]);
      byte masbit = 0;
      os.station_attrib_bits_load(ADDR_NVM_MAS_OP, (byte*)tmp_buffer);  // tmp_buffer now stores masop_bits
      for(sid=0;sid<os.nstations;sid++) {
        // skip if this is the master station
        if (os.status.mas == sid+1) continue;
        bid = sid>>3;
        s = sid&0x07;
        // if this station is running and is set to activate master
        if ((os.station_bits[bid]&(1<<s)) && (tmp_buffer[bid]&(1<<s))) {
          q=pd.queue+pd.station_qid[sid];
          // check if timing is within the acceptable range
          if (curr_time >= q->st + mas_on_adj &&
              curr_time <= q->st + q->dur + mas_off_adj) {
            masbit = 1;
            break;
          }
        }
      }
      os.set_station_bit(os.status.mas-1, masbit);
    }
    // handle master2
    if (os.status.mas2>0) {
      int16_t mas_on_adj_2 = water_time_decode_signed(os.options[OPTION_MASTER_ON_ADJ_2]);
      int16_t mas_off_adj_2= water_time_decode_signed(os.options[OPTION_MASTER_OFF_ADJ_2]);
      byte masbit2 = 0;
      os.station_attrib_bits_load(ADDR_NVM_MAS_OP_2, (byte*)tmp_buffer);  // tmp_buffer now stores masop2_bits
      for(sid=0;sid<os.nstations;sid++) {
        // skip if this is the master station
        if (os.status.mas2 == sid+1) continue;
        bid = sid>>3;
        s = sid&0x07;
        // if this station is running and is set to activate master
        if ((os.station_bits[bid]&(1<<s)) && (tmp_buffer[bid]&(1<<s))) {
          q=pd.queue+pd.station_qid[sid];
          // check if timing is within the acceptable range
          if (curr_time >= q->st + mas_on_adj_2 &&
              curr_time <= q->st + q->dur + mas_off_adj_2) {
            masbit2 = 1;
            break;
          }
        }
      }
      os.set_station_bit(os.status.mas2-1, masbit2);
    }    

    // process dynamic events
    process_dynamic_events(curr_time);

    // activate/deactivate valves
    os.apply_all_station_bits();

    // process LCD display
    if (!ui_state) {
      if(os.get_wifi_mode()==WIFI_MODE_STA && WiFi.status()==WL_CONNECTED && WiFi.localIP()) {
      }
    }
    
    // check safe_reboot condition
    if (os.status.safe_reboot) {
      // if no program is running at the moment
      if (!os.status.program_busy) {
        // and if no program is scheduled to run in the next minute
        bool willrun = false;
        for(pid=0; pid<pd.nprograms; pid++) {
          pd.read(pid, &prog);
          if(prog.check_match(curr_time+60)) {
            willrun = true;
            break;
          }
        }
        if (!willrun) {
          os.reboot_dev();
        }
      }
    }

    // real-time flow count
    static ulong flowcount_rt_start = 0;
    if (os.options[OPTION_SENSOR2_TYPE]==SENSOR_TYPE_FLOW) {
      if (curr_time % FLOWCOUNT_RT_WINDOW == 0) {
        os.flowcount_rt = (flow_count > flowcount_rt_start) ? flow_count - flowcount_rt_start: 0;
        flowcount_rt_start = flow_count;
      }
    }

    // perform ntp sync
    // instead of using curr_time, which may change due to NTP sync itself
    // we use Arduino's millis() method
    //if (curr_time % NTP_SYNC_INTERVAL == 0) os.status.req_ntpsync = 1;
    if((millis()/1000) % NTP_SYNC_INTERVAL==0) os.status.req_ntpsync = 1;
    perform_ntp_sync();

    // check network connection
    if (curr_time && (curr_time % CHECK_NETWORK_INTERVAL==0))  os.status.req_network = 1;
    check_network();

    // check weather
    check_weather();

    byte wuf = os.weather_update_flag;
    if(wuf) {
      if((wuf&WEATHER_UPDATE_EIP) | (wuf&WEATHER_UPDATE_WL)) {
        // at the moment, we only send notification if water level or external IP changed
        // the other changes, such as sunrise, sunset changes are ignored for notification
        push_message(IFTTT_WEATHER_UPDATE, (wuf&WEATHER_UPDATE_EIP)?os.nvdata.external_ip:0,
                                         (wuf&WEATHER_UPDATE_WL)?os.options[OPTION_WATER_PERCENTAGE]:-1);
      }
      os.weather_update_flag = 0;
    }
    static byte reboot_notification = 1;
    if(reboot_notification) {
      reboot_notification = 0;
      push_message(IFTTT_REBOOT);
    }

  }

}

/** Make weather query */
void check_weather() {
  // do not check weather if
  // - network check has failed, or
  // - the controller is in remote extension mode
  if (os.status.network_fails>0 || os.options[OPTION_REMOTE_EXT_MODE]) return;

  if (os.get_wifi_mode()!=WIFI_MODE_STA || WiFi.status()!=WL_CONNECTED || os.state!=OS_STATE_CONNECTED) return;

  ulong ntz = os.now_tz();
  if (os.checkwt_success_lasttime && (ntz > os.checkwt_success_lasttime + CHECK_WEATHER_SUCCESS_TIMEOUT)) {
    // if weather check has failed to return for too long, restart network
    os.checkwt_success_lasttime = 0;
    // mark for safe restart
    os.status.safe_reboot = 1;
    return;
  }
  if (!os.checkwt_lasttime || (ntz > os.checkwt_lasttime + CHECK_WEATHER_TIMEOUT)) {
    os.checkwt_lasttime = ntz;
    GetWeather();
  }
}

/** Turn off a station
 * This function turns off a scheduled station
 * and writes log record
 */
void turn_off_station(byte sid, ulong curr_time) {
  os.set_station_bit(sid, 0);

  byte qid = pd.station_qid[sid];
  // ignore if we are turning off a station that's not running or scheduled to run
  if (qid>=pd.nqueue)  return;

  // RAH implementation of flow sensor
  if (flow_gallons>1) {
    if(flow_stop<=flow_begin) flow_last_gpm = 0;
    else flow_last_gpm = (float) 60000/(float)((flow_stop-flow_begin)/(flow_gallons-1));
  }// RAH calculate GPM, 1 pulse per gallon
  else {flow_last_gpm = 0;}  // RAH if not one gallon (two pulses) measured then record 0 gpm

  RuntimeQueueStruct *q = pd.queue+qid;

  // check if the current time is past the scheduled start time,
  // because we may be turning off a station that hasn't started yet
  if (curr_time > q->st) {
    // record lastrun log (only for non-master stations)
    if(os.status.mas!=(sid+1) && os.status.mas2!=(sid+1)) {
      pd.lastrun.station = sid;
      pd.lastrun.program = q->pid;
      pd.lastrun.duration = curr_time - q->st;
      pd.lastrun.endtime = curr_time;

      // log station run
      write_log(LOGDATA_STATION, curr_time);
      push_message(IFTTT_STATION_RUN, sid, pd.lastrun.duration);
    }
  }

  // dequeue the element
  pd.dequeue(qid);
  pd.station_qid[sid] = 0xFF;
}

/** Process dynamic events
 * such as rain delay, rain sensing
 * and turn off stations accordingly
 */
void process_dynamic_events(ulong curr_time) {
  // check if rain is detected
  bool rain = false;
  bool en = os.status.enabled ? true : false;
  if (os.status.rain_delayed || (os.status.rain_sensed && os.options[OPTION_SENSOR1_TYPE] == SENSOR_TYPE_RAIN)) {
    rain = true;
  }

  byte sid, s, bid, qid, rbits;
  for(bid=0;bid<os.nboards;bid++) {
    rbits = os.station_attrib_bits_read(ADDR_NVM_IGNRAIN+bid);
    for(s=0;s<8;s++) {
      sid=bid*8+s;

      // ignore master stations because they are handled separately      
      if (os.status.mas == sid+1) continue;
      if (os.status.mas2== sid+1) continue;      
      // If this is a normal program (not a run-once or test program)
      // and either the controller is disabled, or
      // if raining and ignore rain bit is cleared
      // FIX ME
      qid = pd.station_qid[sid];
      if(qid==255) continue;
      RuntimeQueueStruct *q = pd.queue + qid;

      if ((q->pid<99) && (!en || (rain && !(rbits&(1<<s)))) ) {
        turn_off_station(sid, curr_time);
      }
    }
  }
}

/** Scheduler
 * This function loops through the queue
 * and schedules the start time of each station
 */
void schedule_all_stations(ulong curr_time) {

  ulong con_start_time = curr_time + 1;   // concurrent start time
  ulong seq_start_time = con_start_time;  // sequential start time

  int16_t station_delay = water_time_decode_signed(os.options[OPTION_STATION_DELAY_TIME]);
  // if the sequential queue has stations running
  if (pd.last_seq_stop_time > curr_time) {
    seq_start_time = pd.last_seq_stop_time + station_delay;
  }

  RuntimeQueueStruct *q = pd.queue;
  byte re = os.options[OPTION_REMOTE_EXT_MODE];
  // go through runtime queue and calculate start time of each station
  for(;q<pd.queue+pd.nqueue;q++) {
    if(q->st) continue; // if this queue element has already been scheduled, skip
    if(!q->dur) continue; // if the element has been marked to reset, skip
    byte sid=q->sid;
    byte bid=sid>>3;
    byte s=sid&0x07;

    // if this is a sequential station and the controller is not in remote extension mode
    // use sequential scheduling. station delay time apples
    if (os.station_attrib_bits_read(ADDR_NVM_STNSEQ+bid)&(1<<s) && !re) {
      // sequential scheduling
      q->st = seq_start_time;
      seq_start_time += q->dur;
      seq_start_time += station_delay; // add station delay time
    } else {
      // otherwise, concurrent scheduling
      q->st = con_start_time;
      // stagger concurrent stations by 1 second
      con_start_time++;
    }
    /*DEBUG_PRINT("[");
    DEBUG_PRINT(sid);
    DEBUG_PRINT(":");
    DEBUG_PRINT(q->st);
    DEBUG_PRINT(",");
    DEBUG_PRINT(q->dur);
    DEBUG_PRINT("]");
    DEBUG_PRINTLN(pd.nqueue);*/
    if (!os.status.program_busy) {
      os.status.program_busy = 1;  // set program busy bit
      // start flow count
      if(os.options[OPTION_SENSOR2_TYPE] == SENSOR_TYPE_FLOW) {  // if flow sensor is connected
        os.flowcount_log_start = flow_count;
        os.sensor_lasttime = curr_time;
      }
    }
  }
}

/** Immediately reset all stations
 * No log records will be written
 */
void reset_all_stations_immediate() {
  os.clear_all_station_bits();
  os.apply_all_station_bits();
  pd.reset_runtime();
}

/** Reset all stations
 * This function sets the duration of
 * every station to 0, which causes
 * all stations to turn off in the next processing cycle.
 * Stations will be logged
 */
void reset_all_stations() {
  RuntimeQueueStruct *q = pd.queue;
  // go through runtime queue and assign water time to 0
  for(;q<pd.queue+pd.nqueue;q++) {
    q->dur = 0;
  }
}


/** Manually start a program
 * If pid==0, this is a test program (1 minute per station)
 * If pid==255, this is a short test program (2 second per station)
 * If pid > 0. run program pid-1
 */
void manual_start_program(byte pid, byte uwt) {
  boolean match_found = false;
  reset_all_stations_immediate();
  ProgramStruct prog;
  ulong dur;
  byte sid, bid, s;
  if ((pid>0)&&(pid<255)) {
    pd.read(pid-1, &prog);
    push_message(IFTTT_PROGRAM_SCHED, pid-1, uwt?os.options[OPTION_WATER_PERCENTAGE]:100, "");
  }
  for(sid=0;sid<os.nstations;sid++) {
    bid=sid>>3;
    s=sid&0x07;
    // skip if the station is a master station (because master cannot be scheduled independently
    if ((os.status.mas==sid+1) || (os.status.mas2==sid+1))
      continue;    
    dur = 60;
    if(pid==255)  dur=2;
    else if(pid>0)
      dur = water_time_resolve(prog.durations[sid]);
    if(uwt) {
      dur = dur * os.options[OPTION_WATER_PERCENTAGE] / 100;
    }
    if(dur>0 && !(os.station_attrib_bits_read(ADDR_NVM_STNDISABLE+bid)&(1<<s))) {
      RuntimeQueueStruct *q = pd.enqueue();
      if (q) {
        q->st = 0;
        q->dur = dur;
        q->sid = sid;
        q->pid = 254;
        match_found = true;
      }
    }
  }
  if(match_found) {
    schedule_all_stations(os.now_tz());
  }
}

// ==========================================
// ====== PUSH NOTIFICATION FUNCTIONS =======
// ==========================================
void ip2string(char* str, byte ip[4]) {
  for(byte i=0;i<4;i++) {
    itoa(ip[i], str+strlen(str), 10);
    if(i!=3) strcat(str, ".");
  }
}

void push_message(byte type, uint32_t lval, float fval, const char* sval) {

  static const char* server = DEFAULT_IFTTT_URL;
  static char key[IFTTT_KEY_MAXSIZE];
  static char postval[TMP_BUFFER_SIZE];

  // check if this type of event is enabled for push notification
  if((os.options[OPTION_IFTTT_ENABLE]&type) == 0) return;
  key[0] = 0;
  read_from_file(ifkey_filename, key);
  key[IFTTT_KEY_MAXSIZE-1]=0;

  if(strlen(key)==0) return;

  strcpy_P(postval, PSTR("{\"value1\":\""));

  switch(type) {

    case IFTTT_STATION_RUN:
      
      strcat_P(postval, PSTR("Station "));
      os.get_station_name(lval, postval+strlen(postval));
      strcat_P(postval, PSTR(" closed. It ran for "));
      itoa((int)fval/60, postval+strlen(postval), 10);
      strcat_P(postval, PSTR(" minutes "));
      itoa((int)fval%60, postval+strlen(postval), 10);
      strcat_P(postval, PSTR(" seconds."));
      if(os.options[OPTION_SENSOR2_TYPE]==SENSOR_TYPE_FLOW) {
        strcat_P(postval, PSTR(" Flow rate: "));
        dtostrf(flow_last_gpm,5,2,postval+strlen(postval));
      }
      break;

    case IFTTT_PROGRAM_SCHED:

      if(sval) strcat_P(postval, PSTR("Manually scheduled "));
      else strcat_P(postval, PSTR("Automatically scheduled "));
      strcat_P(postval, PSTR("Program "));
      {
        ProgramStruct prog;
        pd.read(lval, &prog);
        if(lval<pd.nprograms) strcat(postval, prog.name);
      }
      strcat_P(postval, PSTR(" with "));
      itoa((int)fval, postval+strlen(postval), 10);
      strcat_P(postval, PSTR("% water level."));
      break;

    case IFTTT_RAINSENSOR:

      strcat_P(postval, (lval==LOGDATA_RAINDELAY) ? PSTR("Rain delay ") : PSTR("Rain sensor "));
      strcat_P(postval, ((int)fval)?PSTR("activated."):PSTR("de-activated"));

      break;

    case IFTTT_FLOWSENSOR:
      strcat_P(postval, PSTR("Flow count: "));
      itoa(lval, postval+strlen(postval), 10);
      strcat_P(postval, PSTR(", volume: "));
      {
      uint32_t volume = os.options[OPTION_PULSE_RATE_1];
      volume = (volume<<8)+os.options[OPTION_PULSE_RATE_0];
      volume = lval*volume;
      itoa(volume/100, postval+strlen(postval), 10);
      strcat(postval, ".");
      itoa(volume%100, postval+strlen(postval), 10);
      }
      break;

    case IFTTT_WEATHER_UPDATE:
      if(lval>0) {
        strcat_P(postval, PSTR("External IP updated: "));
        byte ip[4] = {(byte)((lval>>24)&0xFF),
                      (byte)((lval>>16)&0xFF),
                      (byte)((lval>>8)&0xFF),
                      (byte)(lval&0xFF)};
        ip2string(postval, ip);
      }
      if(fval>=0) {
        strcat_P(postval, PSTR("Water level updated: "));
        itoa((int)fval, postval+strlen(postval), 10);
        strcat_P(postval, PSTR("%."));
      }
        
      break;

    case IFTTT_REBOOT:
      strcat_P(postval, PSTR("Rebooted. Device IP: "));
      IPAddress _ip = WiFi.localIP();
      byte ip[4] = {_ip[0], _ip[1], _ip[2], _ip[3]};
      ip2string(postval, ip);
      break;
  }

  strcat_P(postval, PSTR("\"}"));

  //DEBUG_PRINTLN(postval);

  WiFiClient client;
  if(!client.connect(server, 80)) return;
  
  char postBuffer[1500];
  sprintf(postBuffer, "POST /trigger/sprinkler/with/key/%s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "Accept: */*\r\n"
                      "Content-Length: %d\r\n"
                      "Content-Type: application/json\r\n"
                      "\r\n%s", key, server, strlen(postval), postval);
  client.write((uint8_t *)postBuffer, strlen(postBuffer));

  time_t timeout = os.now_tz() + 5; // 5 seconds timeout
  while(!client.available() && os.now_tz() < timeout) {
  }

  bzero(ether_buffer, ETHER_BUFFER_SIZE);
  
  while(client.available()) {
    client.read((uint8_t*)ether_buffer, ETHER_BUFFER_SIZE);
  }
  client.stop();
  //DEBUG_PRINTLN(ether_buffer);
}

// ================================
// ====== LOGGING FUNCTIONS =======
// ================================
char LOG_PREFIX[] = "/logs/";

/** Generate log file name
 * Log files will be named /logs/xxxxx.txt
 */
void make_logfile_name(char *name) {
  strcpy(tmp_buffer+TMP_BUFFER_SIZE-10, name);
  strcpy(tmp_buffer, LOG_PREFIX);
  strcat(tmp_buffer, tmp_buffer+TMP_BUFFER_SIZE-10);
  strcat_P(tmp_buffer, PSTR(".txt"));
}

/* To save RAM space, we store log type names
 * in program memory, and each name
 * must be strictly two characters with an ending 0
 * so each name is 3 characters total
 */
static const char log_type_names[] PROGMEM =
    "  \0"
    "rs\0"
    "rd\0"
    "wl\0"
    "fl\0";

/** write run record to log on SD card */
void write_log(byte type, ulong curr_time) {

  if (!os.options[OPTION_ENABLE_LOGGING]) return;

  // file name will be logs/xxxxx.tx where xxxxx is the day in epoch time
  ultoa(curr_time / 86400, tmp_buffer, 10);
  make_logfile_name(tmp_buffer);

  // Step 1: open file if exists, or create new otherwise, 
  // and move file pointer to the end  
  // prepare log folder for Arduino
  if (!os.status.has_sd)  return;
  File file = SPIFFS.open(tmp_buffer, "r+");
  if(!file) {
    file = SPIFFS.open(tmp_buffer, "w");
    if(!file) return;
  }
  file.seek(0, SeekEnd);
  // prepare log folder
  
  // Step 2: prepare data buffer
  strcpy_P(tmp_buffer, PSTR("["));

  if(type == LOGDATA_STATION) {
    itoa(pd.lastrun.program, tmp_buffer+strlen(tmp_buffer), 10);
    strcat_P(tmp_buffer, PSTR(","));
    itoa(pd.lastrun.station, tmp_buffer+strlen(tmp_buffer), 10);
    strcat_P(tmp_buffer, PSTR(","));
    // duration is unsigned integer
    ultoa((ulong)pd.lastrun.duration, tmp_buffer+strlen(tmp_buffer), 10);
  } else {
    ulong lvalue=0;
    if(type==LOGDATA_FLOWSENSE) {
      lvalue = (flow_count>os.flowcount_log_start)?(flow_count-os.flowcount_log_start):0;
    }
    ultoa(lvalue, tmp_buffer+strlen(tmp_buffer), 10);
    strcat_P(tmp_buffer, PSTR(",\""));
    strcat_P(tmp_buffer, log_type_names+type*3);
    strcat_P(tmp_buffer, PSTR("\","));

    switch(type) {
      case LOGDATA_RAINSENSE:
      case LOGDATA_FLOWSENSE:
        lvalue = (curr_time>os.sensor_lasttime)?(curr_time-os.sensor_lasttime):0;
        break;
      case LOGDATA_RAINDELAY:
        lvalue = (curr_time>os.raindelay_start_time)?(curr_time-os.raindelay_start_time):0;
        break;
      case LOGDATA_WATERLEVEL:
        lvalue = os.options[OPTION_WATER_PERCENTAGE];
        break;
    }
    ultoa(lvalue, tmp_buffer+strlen(tmp_buffer), 10);
  }
  strcat_P(tmp_buffer, PSTR(","));
  ultoa(curr_time, tmp_buffer+strlen(tmp_buffer), 10);
  if((os.options[OPTION_SENSOR2_TYPE]==SENSOR_TYPE_FLOW) && (type==LOGDATA_STATION)) {
    // RAH implementation of flow sensor
    strcat_P(tmp_buffer, PSTR(","));
    dtostrf(flow_last_gpm,5,2,tmp_buffer+strlen(tmp_buffer));
  }
  strcat_P(tmp_buffer, PSTR("]\r\n"));
  file.write((byte*)tmp_buffer, strlen(tmp_buffer));
}


/** Delete log file
 * If name is 'all', delete all logs
 */
void delete_log(char *name) {
  if (!os.options[OPTION_ENABLE_LOGGING]) return;
  if (!os.status.has_sd) return;

  if (strncmp(name, "all", 3) == 0) {
    // delete all log files
    Dir dir = SPIFFS.openDir(LOG_PREFIX);
    while (dir.next()) {
      SPIFFS.remove(dir.fileName());
    }
  } else {
    // delete a single log file
    make_logfile_name(name);
    if(!SPIFFS.exists(tmp_buffer)) return;
    SPIFFS.remove(tmp_buffer);
  }
}

/** Perform network check
 * This function pings the router
 * to check if it's still online.
 * If not, it re-initializes Ethernet controller.
 */
void check_network() {

  // nothing to do for other platforms
}

/** Perform NTP sync */
void perform_ntp_sync() {
  // do not perform sync if this option is disabled, or if network is not available, or if a program is running
  if (!os.options[OPTION_USE_NTP] || os.status.program_busy) return;
  if (os.get_wifi_mode()!=WIFI_MODE_STA || WiFi.status()!=WL_CONNECTED || os.state!=OS_STATE_CONNECTED) return;

  if (os.status.req_ntpsync) {
    // check if rtc is uninitialized
    // 978307200 is Jan 1, 2001, 00:00:00
    boolean rtc_zero = (now()<=978307200L);
    
    os.status.req_ntpsync = 0;
    if (!ui_state) {
      os.lcd_print_line_clear_pgm(PSTR("NTP Syncing..."),1);
    }
    ulong t = getNtpTime();
    if (t>0) {
      setTime(t);
    }
  }
}
