// supplemental chicken coop light controller

// (c) 2016 Joshua Heling <jrh@netfluvia.org>
// BSD license

#include <Wire.h>
#include <EEPROM.h>
#include "RTClib.h"

//////////////////////////////////////////////// log helper macros

// defining these enables the log_<level>() functions
//#define LOG_TRACE
#define LOG_DEBUG 
#define LOG_WARN
#define LOG_ERROR

#ifdef LOG_TRACE
#define log_trace(s) _log("DEBUG", s)
#else 
#define log_trace(s)
#endif

#ifdef LOG_DEBUG
#define log_debug(s) _log("DEBUG", s)
#else 
#define log_debug(s)
#endif

#ifdef LOG_WARN
#define log_warn(s) _log("WARN", s)
#else 
#define log_warn(s)
#endif

#ifdef LOG_ERROR
#define log_error(s) _log("ERROR", s)
#else 
#define log_error(s)
#endif

//////////////////////////////////////////////// end log helper macros

//////////////////////////////////////////////// hw-specific config

#define REDPIN 5      // digital output pins connected to the LED strip
#define GREENPIN 6
#define BLUEPIN 3

#define LDRPIN 0      // analog input used for light-dependent resistor

//////////////////////////////////////////////// end hw-specific config

//////////////////////////////////////////////// globals

RTC_DS1307 rtc;

#define SUNRISE 0     // do not changed - used to make references into the sunchange array readable
#define SUNSET 1

typedef struct {
  String str;
  unsigned long interval; // s since midnight
  byte ee_addr;
  unsigned long minval;
  unsigned long maxval;
  unsigned long defval;
} sunchange_t;

// "sunchange" refers to sunset or sunrise - sunchange_t tracks what we know about them
//  (refactoring note: consider making this a class, since it's a struct with at least one non-trivial
//   function that interacts almost completely with it.)
//
// Note:
//  * min/max/default sunchange values are somewhat location-specific - defaults below
//     are appropriate for the US Midwest - change as needed
//  * all sunrise/sunset vars are intervals (seconds since midnight), not timestamps of any sort.
//      (NB: this would break if the sun rose before or set after midnight.  
//       You're on your own, extreme latitudes)
//  * Yes, it would be possible to use a data table of sunrise info (which could be localized) as an 
//      alternative, but tracking actual light values was chosen instead to account for hyper-local
//      effects (e.g. valleys, extreme shadows, etc.) that would be missed in that approach.
sunchange_t sunchange[2] = {
  {
    "sunrise",  
    0UL,      // moment of sunchange - 0 indicates default startup state
    10,       // mem addr in eeprom
    18000,    // min sunrise is 5am
    28800,    // max sunrise is 8am
    21600     // default sunrise is 6am
  },
  {
    "sunset",  
    0UL,
    14,       // mem addr in eeprom
    57600,    // min sunset is 4pm
    75600,    // max sunset is 9pm
    61200     // default sunset is 5pm
  }
};

// these are used to track runtime state - do not change
bool sunIsUp = false;           // true if we think it's naturally light out now
unsigned long natural_light_falling_since = 0;  // track debounce for sunset
unsigned long natural_light_rising_since = 0;   // track debounce for sunrise
unsigned long start_supplemental_light = 0;   // time (seconds since midnight) to start supplementing

///////////////////// configurable bits
/////////////////////
/////////////////////
unsigned long sunchange_thresh = 300;  // If observed values are >5m off of recorded values, update the EEPROM
                                       //     This limits EEPROM write cycles; at a 5m threshold the EEPROM should 
                                       //     last longer than you will.
unsigned long max_sunchange = 600;     // Never change sunchange more than 10m at a time.  Protects against 
                                       //     unexpected conditions massively changing state in one day.

unsigned int total_light_minutes = 900;    // how many minutes of light (supplemental + natural) we want (15h)
unsigned long dimming_time = 1200;         // duration of slow fade in/out of light.  This happens before/after
                                           //   the supplemental light period

int max_power = 128;                 // LED strings are fully on at 256 (but also use the most power then)

const int natural_light_lowThresh = 700;  // photoresistor value below which we consider dark - FIXME: 150
const int natural_light_debounce_s = 60;  // number of seconds we need to see above/below threshold before 
                                          //   we conclude natural light has changed

//////////////////////////////////////////////// end globals

// write a timestamped log to Serial, prefaced with specified level
//
// nb: intended to be referenced from preproc macro wrapper
void _log(const String& level, const String& msg) {
  String t;
  humanTimestamp(t);
  Serial.print(t);
  Serial.print(" ");
  Serial.print(level);
  Serial.print(" ");
  Serial.println(msg);
}

unsigned long secondsSinceMidnight() {
  DateTime now = rtc.now();
  return (((unsigned long)now.hour() * (60 * 60)) + ((unsigned long)now.minute() * 60) + (unsigned long)now.second());
}

// put timestamp in form "YYYY-MM-DD HH:MM:SS" into argument
//
// NB: this might be materially memory inefficient
void humanTimestamp(String &ts) {  
  DateTime now = rtc.now();
  ts = now.year();
  ts += '-';
  ts += now.month();
  ts += '-';
  ts += now.day();
  ts += ' ';
  ts += now.hour();
  ts += ':';
  ts += now.minute();
  ts += ':';
  ts += now.second();
}

void write_ul_to_EEPROM(int addr, unsigned long val) {  
  byte* p = (byte*)&val;  
  for (int i = 0; i < sizeof(val); i++) {    
    EEPROM.write(addr + i, p[i]);
  }
}

void read_ul_from_EEPROM(int addr, unsigned long &val) {  
  byte* p = (byte*)&val;
  for (int i = 0; i < sizeof(val); i++) {
    p[i] = EEPROM.read(addr+i);
  }
}

// turns on R, G, and B LEDs to the specified power level
void led_power(int p) {
  if (p < 0) {
    p = 0;
  }
  if (p > 255) {
    p = 255;
  }

  String s;
  s = "LED power: ";
  s += p;
  log_debug(s);

  analogWrite(GREENPIN, p);
  analogWrite(REDPIN, p);
  analogWrite(BLUEPIN, p);
}

// called at init at when sunchange values change
// side effect: updates start_supplemental_light
void update_light_timing() {
  // We want to make sure we get at least total_light_minutes minutes of light. 
  // 
  // But it's good practice to add this light to the beginning of the day, so the chickens get a natural
  //  sunset and other signals to tell them to return to the roost before it's too dark to do so.
  String s;
  unsigned long start_supp;
  long supp_light_duration;
  
  unsigned long natural_light = sunchange[SUNSET].interval - sunchange[SUNRISE].interval;
  s = "Natural light provides ";
  s += natural_light;
  s += " seconds of light.";
  log_debug(s);

  supp_light_duration = (total_light_minutes * 60) - natural_light;
  s = "Supplemental light is needed for ";
  s += supp_light_duration;
  s += " seconds each day.";
  log_debug(s);
  
  if (supp_light_duration > 0) {   
    start_supp = sunchange[SUNRISE].interval - supp_light_duration;
    s = "start_supp = ";
    s += start_supp;
    log_debug(s);
    if ((start_supp) < 0) {
      // we need to start supplemental lighting before midnight
      start_supplemental_light = 86400 + start_supp;  // start_supp is negative, 86400 is midnight next
    } else {
      start_supplemental_light = start_supp;
    }
    s = "Setting supplemental light to start ";
    s += start_supplemental_light;
    s += " seconds into the day";
    log_debug(s);
  } else {
    log_debug("No supplemental light needed.");
  }  
}

// update_sunchange() is called:
//     a) upon initialization
//     b) when the sun has been observed rising or setting
//
// "sunchange" refers to either a sunrise or sunset.  Our manipulation of them is the same 
//   in both cases.
//
// NB: all sunchange values are measured as the interval (s) since midnight
//
// Parameters: 
//    * event - either SUNRISE or SUNSET (defined as indexes into the struct array)
//    * observed sunchange interval.  Value 0 is considered a special case,
//                and will not be treated as actual sunchange.
//
// Side effects:
//  1) In all cases, this will set the interval field of the struct to our _effective_ sunchange time
//  2) If the interval is 0 (init case), we set sunchange from EEPROM.  (Else the global should match
//       the EEPROM-stored value.)
//  3) If the EEPROM has values outside the defined range of possibility, EEPROM is updated
//       with the default.
//  4) If the observed value varies from the stored value by more than the threshold, then
//       the stored value is updated to match.  (This threshold behavior reduces the write cycles
//       consumed on the EEPROM.)
//  5) Will call update_light_timing() when the sunchange intervals change.
//
void update_sunchange(int event, unsigned long observed) {
  String s;
  if ((event != SUNRISE) and (event != SUNSET)) {    
    s = "Invalid event ";
    s += event;
    s += " passed to update_sunchange - DOING NOTHING";
    log_error(s);
  }
  
  if (sunchange[event].interval == 0) {
    // happens at initial startup - need to get stored value from EEPROM
    read_ul_from_EEPROM(sunchange[event].ee_addr, sunchange[event].interval);
    s = sunchange[event].str;
    s += " read from EEPROM: ";
    s += sunchange[event].interval;
    log_debug(s);

    // validity check
    if ((sunchange[event].interval < sunchange[event].minval) or (sunchange[event].interval > sunchange[event].maxval)) {
      sunchange[event].interval = sunchange[event].defval;      
      s = sunchange[event].str;
      s += " EEPROM value was out of range; using default of ";
      s += sunchange[event].defval;
      s += " instead.";
      log_debug(s);
      write_ul_to_EEPROM(sunchange[event].ee_addr, sunchange[event].interval);
      update_light_timing();
    }
  }

  
  if (observed > 0) {
    // sanity-check: make sure we never set sunrise to be after sunset
    if (((event == SUNRISE) and (observed >= sunchange[SUNSET].interval)) or
        ((event == SUNSET) and (observed <= sunchange[SUNRISE].interval))) {
      log_error("Ignoring attempt to set sunrise impossibly after sunset - something is wrong.");      
      return;
    }
    
    if (abs(observed - sunchange[event].interval) > sunchange_thresh) {      
      s = "Updating EEPROM-stored ";
      s += sunchange[event].str;
      s += " value.";
      log_warn(s);      
      if (observed < sunchange[event].minval) {                   
        // no matter what, don't set the change below the defined minimum ...
        s = "Clamping below-min ";
        s += sunchange[event].str;
        s += " value ";
        s += observed;
        s = " to defined minimum ";
        s += sunchange[event].minval;
        log_warn(s);
        observed = sunchange[event].minval;
      } else if (observed > sunchange[event].maxval) {        
        // ... or above the defined maximum ... 
        s = "Clamping above-max ";
        s += sunchange[event].str;
        s += " value ";
        s += observed;
        s = " to defined maximum ";
        s += sunchange[event].maxval;
        log_warn(s);
        observed = sunchange[event].maxval;
      } else if (abs(sunchange[event].interval - observed) > max_sunchange) {
        // ... or move too much in a single step
        s = "Limiting big single step ";
        s += sunchange[event].str;
        s += " change (";
        s += sunchange[event].interval;
        s += " to ";
        s += observed;        
        s += " to max single-step change of ";
        s += max_sunchange;
        log_warn(s);
        if ((sunchange[event].interval - observed) < 0) {
          // we saw change earlier in the day than previous interval
          observed = sunchange[event].interval - max_sunchange;
        } else {
          // we saw change later in the day than previous interval
          observed = sunchange[event].interval + max_sunchange;
        }
      }
      sunchange[event].interval = observed;      
      s = sunchange[event].str;
      s += " value is now ";
      s += sunchange[event].interval;
      log_warn(s);
      write_ul_to_EEPROM(sunchange[event].ee_addr, sunchange[event].interval);
      update_light_timing();
    }
    // do nothing if the observed value is within the threshold from the current value
  }  
}

// checkLightLevel() - read LDR and update understanding of sunrise/sunset accordingly
//
// side effects:
//   * update sunIsUp
//   * update sunrise/sunset times 
void checkLightLevel() {
  int _lightVal = analogRead(LDRPIN);
  String s = "lightlevel: ";
  s += _lightVal;
  log_trace(s);

  if (sunIsUp) {      // We're waiting for the sun to go down
    if (_lightVal < natural_light_lowThresh) {   // seems to be dark   
      if (natural_light_falling_since > 0) {     // we're waiting for the long debounce to finish
        log_trace("Still dark - waiting for sunset debounce to complete...");
        if ((secondsSinceMidnight() - natural_light_falling_since) > natural_light_debounce_s) {  // sun has set
          log_trace("   *** sunset debounce is complete!");
          log_warn("sun has set");
          sunIsUp = false;
          natural_light_falling_since = 0;
          update_sunchange(SUNSET, secondsSinceMidnight());  // updates global & stores new val in EEPROM if appropriate
        }        
      } else {                                    // this is the first dark we've seen - start debounce timer
        log_warn("Saw first dark - starting to wait for sunset debounce to complete.");
        natural_light_falling_since = secondsSinceMidnight();
      }      
    } else {
      natural_light_falling_since = 0;            // reset debounce timer on the falling side, in case we just bounced      
      log_trace("still light out");
    }    
  } else {            // Sun is currently down; we're waiting for it to come up
    if (_lightVal >= natural_light_lowThresh) {   // seems to be light      
      if (natural_light_rising_since > 0) {      // we're waiting for the long debounce to finish
        log_trace("Still light - waiting for sunrise debounce to complete...");
        if ((secondsSinceMidnight() - natural_light_rising_since) > natural_light_debounce_s) {  // sun has risen
          log_trace("   *** sunrise debounce is complete!");
          log_warn("sun has risen");
          sunIsUp = true;
          natural_light_rising_since = 0;
          update_sunchange(SUNRISE, secondsSinceMidnight());  // updates global & stores new val in EEPROM if appropriate
        }        
      } else {                                    // this is the first light we've seen - start debounce timer
        log_warn("Saw first light - starting to wait for sunrise debounce to complete.");
        natural_light_rising_since = secondsSinceMidnight();
      }      
    } else {
      natural_light_rising_since = 0;            // reset debounce timer on the rising side, in case we just bounced   
      log_trace("still dark out");
    }        
  }
  
}

void setup() {
  Serial.begin(9600);  

  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  pinMode(REDPIN, OUTPUT);
  pinMode(GREENPIN, OUTPUT);
  pinMode(BLUEPIN, OUTPUT);

  // init sunrise/sunset data from EEPROM
  update_sunchange(SUNRISE, 0);
  update_sunchange(SUNSET, 0);         
  update_light_timing();

  // refactoring note: clean up tests
  // (NB: these "tests" are intended to be momentarily uncommented, uploaded, and run while watching
  //  Serial output.  The testing all happens from setup() - letting the sketch run with one of these tests
  //  uncommented won't produce meaningful behavior.)
  
  // TEST: should conclude no supplemental light needed
//  update_sunchange(SUNRISE, 18000);
//  update_sunchange(SUNSET, 75600);         
//  update_light_timing();

  // TEST: should conclude 7200s supplemental needed, turning on at 21600
//  update_sunchange(SUNRISE, 28800);
//  update_sunchange(SUNSET, 75600);         
//  update_light_timing();

  // TEST cleanup: write defaults to eeprom
  // Note: this should be run after running any of the tests, and will then allow normal operation to eventually
  //   re-converge on accurate sunchange values.  The rate of reconvergence is limited by max_sunchange.  
  //   Alternatively, you could manually set the sunchange values to something close to accurate for your 
  //   time and place after running tests.
//  write_ul_to_EEPROM(sunchange[SUNRISE].ee_addr, sunchange[SUNRISE].defval);  
//  write_ul_to_EEPROM(sunchange[SUNSET].ee_addr, sunchange[SUNSET].defval);
//  Serial.println("restored default sunrise & sunset values - exiting");
//  delay(500); // wait for serial write to finish
//  exit(1);
  
  // flash green twice to confirm startup complete    
  for (int i = 0; i < 2; i++) {
    analogWrite(GREENPIN, 128);
    delay(500);
    analogWrite(GREENPIN, 0);
    delay(200);
  }  

  // figure out if the sun is up now or not
  unsigned long ssm = secondsSinceMidnight();
  if ((ssm > sunchange[SUNRISE].interval) and
      (ssm < sunchange[SUNSET].interval)) {
    sunIsUp = true;
  }
}

// rename to loop() and watch serial output to confirm LDR readings
void LDR_test_loop() {
  int _lightVal = analogRead(LDRPIN);
  String s = "lightlevel: ";
  s += _lightVal;
  log_debug(s);
  delay(1000);
}

void loop() {  
  checkLightLevel();  
  unsigned long ssm = secondsSinceMidnight(); 

  if (ssm >= (sunchange[SUNRISE].interval + dimming_time)) {
    // light is fully off
    led_power(0);
    delay(60000); 
  } else if (ssm >= sunchange[SUNRISE].interval) {
    // light is dimming down
    // power level should be at max in the beginning of the dimming_time and 0 at the end
    // (ssm - sunchange[SUNRISE].interval) ==> how far into the dimming_time we are
    led_power(((float)(dimming_time - (ssm - sunchange[SUNRISE].interval)) / dimming_time) * max_power);
    delay(1000);
  } else if (ssm >= start_supplemental_light) {
    // light is on at full power
    led_power(max_power);
    delay(60000);
  } else if (ssm >= (start_supplemental_light - dimming_time)) {
    // light is gradually increasing
    // power level should be at 0 in the beginning of the dimming_time and max at the end
    
    // FIXME: bug here if start_supplemental_light is less than dimming_time from midnight (0)?
    led_power(((float)(ssm - (start_supplemental_light - dimming_time)) / dimming_time) * max_power);
    delay(1000);
  } else {
    // light is off
    led_power(0);
    delay(60000);
  }      
}
