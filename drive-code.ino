#define debug

#include <SPI.h>
#include <esp32-hal-ledc.h>
#include <Wire.h>
#include "nunchuk.h"
#include <math.h>

// Motor PWM pins
#define PWMLeft 32
#define PWMRight 33
#define nConnect 4

// Motor control dir control pins
#define motorLeftDir 26  // Left motor direction control pin
#define motorRightDir 27 // Right motor direction control pin
// Motor control enable pin (optional, can be used to disable motors)
#define motorEnable 25 // Enable pin for motor driver
// Motor control parameters
const int speedmax = 4095;                  // 12-bit integer (const for safety)
const int speedmin = 512;                   // 1/8 of 12-bit int
const int deadzone = 20;                    // Deadzone for nunchuk joystick
const int SCALING_FACTOR = 16;              // Joystick scaling factor
const unsigned long FAILSAFE_TIMEOUT = 500; // ms - stop motors if no valid reading
const unsigned long SERIAL_INTERVAL = 250;  // ms - status printing interval

// Improved motor ramping for smoother control
const int MAX_SPEED_CHANGE_NORMAL = 150;    // Normal acceleration/deceleration per cycle
const int MAX_SPEED_CHANGE_EMERGENCY = 800; // Emergency stop deceleration per cycle
const int MAX_SPEED_CHANGE_STARTUP = 100;   // Gentler startup acceleration
const int STARTUP_THRESHOLD = 200;          // Speed below which startup ramping applies
const int MINIMUM_UPDATE_THRESHOLD = 5;     // Minimum change to update PWM

// Advanced safety parameters
const unsigned long EMERGENCY_STOP_HOLD_TIME = 1000; // ms - how long to hold emergency stop
const int DIRECTION_CHANGE_DELAY = 50;               // ms - delay when changing direction
const int CONNECTION_CHECK_INTERVAL = 100;           // ms - how often to check connection

// Connection monitoring
unsigned long lastValidRead = 0;
unsigned long lastSerialPrint = 0;
unsigned long emergencyStopTime = 0;
unsigned long lastDirectionChange = 0;
bool connectionLost = false;
bool emergencyStopActive = false;

// Motor state tracking for improved ramping
struct motorState
{
    int currentSpeed;
    int targetSpeed;
    bool currentDir;
    bool targetDir;
    unsigned long lastUpdate;
};

motorState leftMotor = {0, 0, 0, 0, 0};
motorState rightMotor = {0, 0, 0, 0, 0};

// Simplified droid state structure (removed dome-related fields)
struct droidstate
{
    int left;
    int right;
    bool leftdir;
    bool rightdir;
    bool driveEN;
    bool motorsEngaged; // Z button state for motor engagement
    bool emergencyStop; // Emergency stop state
};

droidstate droid, newdroid;
const droidstate droidzero = {0, 0, 0, 0, 1, 0, 0}; // Default state with motors disabled

// Function declarations
void reset();
void processNunchukInput();
void processDriveInput();
void update();
void printStatus();
void checkFailsafe();
bool initNunchukWithRetry();
int getMagnitude(int cx, int cy);
int calculateRampedSpeed(int targetSpeed, int currentSpeed, bool isEmergencyStop = false, bool isStartup = false);
void updateMotorWithRamping(int motorPin, motorState &motor, int targetSpeed, bool targetDir);
bool checkDirectionChange(bool currentDir, bool targetDir);
void handleEmergencyStop();

void setup()
{
    Serial.begin(115200);
    Wire.begin(); // Default i2c pins (21, 22)

    // Initialize nunchuk with retry logic
    int initAttempts = 0;
    while (!initNunchukWithRetry() && initAttempts < 5)
    {
        initAttempts++;
        delay(100);
    }

    if (initAttempts >= 5)
    {
        Serial.println("WARNING: Nunchuk initialization failed!");
    }

    // Initialize PWM channels for motors
    ledcAttach(PWMLeft, 1000, 12);
    ledcAttach(PWMRight, 1000, 12);

    // Initialize motor direction and enable pins
    pinMode(motorLeftDir, OUTPUT);
    pinMode(motorRightDir, OUTPUT);
    pinMode(motorEnable, OUTPUT);

    // Set initial safe states
    digitalWrite(motorLeftDir, LOW);  // Forward direction initially
    digitalWrite(motorRightDir, LOW); // Forward direction initially
    digitalWrite(motorEnable, LOW);   // Motors disabled initially

    // Initialize nunchuk connection pin
    pinMode(nConnect, INPUT_PULLDOWN);

    // Initialize droid state
    reset();

    Serial.println("Nunchuk Drive Control Initialized v2.0");
    Serial.println("Hold Z button to engage motors, C button for emergency stop");
    Serial.println("Features: Smooth ramping, direction protection, enhanced safety, GPIO control");

    lastValidRead = millis();
    lastSerialPrint = millis();
}

void loop()
{
    unsigned long currentTime = millis();

    // Read nunchuk data
    if (nunchuk_read())
    {
        lastValidRead = currentTime;
        connectionLost = false;

        // Process nunchuk input if connected
        if (digitalRead(nConnect))
        {
            // Nunchuk is connected, process commands
            processNunchukInput();
        }
        else
        {
            // Nunchuk disconnected, stop motors
            if (!connectionLost)
            {
                Serial.println("Nunchuk disconnected");
                connectionLost = true;
            }
            reset();
        }
    }
    else
    {
        // Failed to read nunchuk, check failsafe
        checkFailsafe();
    }

    // Handle emergency stop timing
    handleEmergencyStop();

    // Update motor outputs with advanced ramping
    update();

    // Print status periodically instead of every loop
    if (currentTime - lastSerialPrint >= SERIAL_INTERVAL)
    {
#ifdef debug
        printStatus();
#endif
        lastSerialPrint = currentTime;
    }

    // Copy new state to current state
    droid = newdroid;

    delay(10);
}

bool initNunchukWithRetry()
{
    nunchuk_init();
    delay(50);

    // Try to read to verify initialization
    return nunchuk_read();
}

void checkFailsafe()
{
    unsigned long timeSinceLastRead = millis() - lastValidRead;

    if (timeSinceLastRead > FAILSAFE_TIMEOUT)
    {
        if (!connectionLost)
        {
            Serial.println("FAILSAFE: Communication timeout - stopping motors");
            connectionLost = true;
        }
        reset();
    }
}

void reset()
{
    // Reset to safe state
    newdroid = droidzero;

    // Don't automatically trigger emergency stop on reset unless it's a real emergency
    // Only set emergency stop if we're resetting due to connection loss
    if (connectionLost)
    {
        newdroid.emergencyStop = true;
        emergencyStopActive = true;
        emergencyStopTime = millis();
    }
    else
    {
        // Normal reset - just stop motors but don't trigger emergency stop hold
        newdroid.emergencyStop = false;
        emergencyStopActive = false;
    }

    // Reset motor states for clean ramping
    leftMotor.targetSpeed = 0;
    rightMotor.targetSpeed = 0;
    leftMotor.targetDir = 0;
    rightMotor.targetDir = 0;
    leftMotor.currentSpeed = 0;
    rightMotor.currentSpeed = 0;

    if (!connectionLost)
    {
        Serial.println("System reset - motors ready");
    }
}

void handleEmergencyStop()
{
    if (emergencyStopActive)
    {
        unsigned long timeSinceStop = millis() - emergencyStopTime;

        // Keep emergency stop active for minimum duration
        if (timeSinceStop >= EMERGENCY_STOP_HOLD_TIME)
        {
            // Only clear if C button is not pressed and motors are stopped
            if (!nunchuk_buttonC() && leftMotor.currentSpeed == 0 && rightMotor.currentSpeed == 0)
            {
                emergencyStopActive = false;
                newdroid.emergencyStop = false;
                Serial.println("Emergency stop cleared - system ready");
            }
        }

        // Force motors to stop during emergency stop
        newdroid.left = 0;
        newdroid.right = 0;
        newdroid.driveEN = 1; // Disable motors
    }
}

void processNunchukInput()
{
    // Check for emergency stop first
    if (nunchuk_buttonC())
    {
        if (!emergencyStopActive)
        {
            Serial.println("EMERGENCY STOP activated (C button)!");
            emergencyStopActive = true;
            emergencyStopTime = millis();
        }
        newdroid.emergencyStop = true;
        return;
    }

    // Don't process other inputs during emergency stop hold time
    if (emergencyStopActive)
    {
        newdroid.emergencyStop = true;
        return;
    }

    // Check Z button for motor engagement (must be held to engage)
    newdroid.motorsEngaged = nunchuk_buttonZ();

    // Only process drive commands if Z button is held and no emergency stop
    if (newdroid.motorsEngaged && !newdroid.emergencyStop && !emergencyStopActive)
    {
        processDriveInput();
    }
    else
    {
        // Z button not held or emergency stop active, disable motors
        newdroid.left = 0;
        newdroid.right = 0;
        newdroid.driveEN = 1; // Disable motors
    }
}

void processDriveInput()
{
    // Get nunchuk joystick values (-128 to 127)
    int joyX = nunchuk_joystickX();
    int joyY = nunchuk_joystickY();

    // Apply individual axis deadzone
    if (abs(joyX) < deadzone)
        joyX = 0;
    if (abs(joyY) < deadzone)
        joyY = 0;

    // Check if joystick is in overall deadzone
    if (getMagnitude(joyX, joyY) < deadzone)
    {
        newdroid.left = 0;
        newdroid.right = 0;
        // DON'T disable motors here - let update() handle enable logic
        return;
    }

    // Calculate tank steering values with improved scaling
    int forward = joyY * SCALING_FACTOR;
    int turn = joyX * SCALING_FACTOR;

    // Tank steering: when turning, motors move in opposite directions
    int leftSpeed, rightSpeed;

    if (abs(turn) > abs(forward))
    {
        // Pure turning mode - motors move in opposite directions
        // For left turn (negative X): left motor backward, right motor forward
        // For right turn (positive X): left motor forward, right motor backward
        leftSpeed = turn;   // Left motor: same as turn direction
        rightSpeed = -turn; // Right motor: opposite of turn direction
    }
    else
    {
        // Mixed mode - forward/backward with turning
        leftSpeed = forward + turn;  // Left motor: forward + turn
        rightSpeed = forward - turn; // Right motor: forward - turn
    }

    // Set motor directions based on calculated speeds
    bool newLeftDir = (leftSpeed >= 0) ? 0 : 1;   // 0 = forward, 1 = reverse
    bool newRightDir = (rightSpeed >= 0) ? 0 : 1; // 0 = forward, 1 = reverse

    // Check for direction changes and apply safety delay
    unsigned long currentTime = millis();
    if (checkDirectionChange(newdroid.leftdir, newLeftDir) ||
        checkDirectionChange(newdroid.rightdir, newRightDir))
    {
        if (currentTime - lastDirectionChange < DIRECTION_CHANGE_DELAY)
        {
            // Too soon for direction change, maintain current values
            return;
        }
        lastDirectionChange = currentTime;
        Serial.println("Direction change detected - applying safety delay");
    }

    newdroid.leftdir = newLeftDir;
    newdroid.rightdir = newRightDir;

    // Calculate target speeds (absolute values), constrain to valid range
    int targetLeft = constrain(abs(leftSpeed), 0, speedmax);
    int targetRight = constrain(abs(rightSpeed), 0, speedmax);

    // Apply minimum speed threshold
    if (targetLeft > 0 && targetLeft < speedmin)
        targetLeft = speedmin;
    if (targetRight > 0 && targetRight < speedmin)
        targetRight = speedmin;

    // Store target speeds (ramping will be applied in update())
    newdroid.left = targetLeft;
    newdroid.right = targetRight;

// Debug output
#ifdef debug
    Serial.print("Joy X: ");
    Serial.print(joyX);
    Serial.print(", Y: ");
    Serial.print(joyY);
    Serial.print(" | Forward: ");
    Serial.print(forward);
    Serial.print(", Turn: ");
    Serial.print(turn);
    Serial.print(" | LeftSpeed: ");
    Serial.print(leftSpeed);
    Serial.print(", RightSpeed: ");
    Serial.print(rightSpeed);
    Serial.print(" | Target L: ");
    Serial.print(targetLeft);
    Serial.print(", R: ");
    Serial.print(targetRight);
    Serial.print(" | Ramped L: ");
    Serial.print(leftMotor.currentSpeed);
    Serial.print(" (dir: ");
    Serial.print(newdroid.leftdir);
    Serial.print("), R: ");
    Serial.print(rightMotor.currentSpeed);
    Serial.print(" (dir: ");
    Serial.print(newdroid.rightdir);
    Serial.println();
#endif
}

bool checkDirectionChange(bool currentDir, bool targetDir)
{
    return (currentDir != targetDir);
}

int calculateRampedSpeed(int targetSpeed, int currentSpeed, bool isEmergencyStop, bool isStartup)
{
    int speedDiff = targetSpeed - currentSpeed;
    int maxChange;

    // Determine maximum allowed speed change based on conditions
    if (isEmergencyStop)
    {
        maxChange = MAX_SPEED_CHANGE_EMERGENCY;
    }
    else if (isStartup && currentSpeed < STARTUP_THRESHOLD)
    {
        maxChange = MAX_SPEED_CHANGE_STARTUP;
    }
    else
    {
        maxChange = MAX_SPEED_CHANGE_NORMAL;
    }

    // Apply ramping
    if (abs(speedDiff) <= maxChange)
    {
        return targetSpeed;
    }
    else
    {
        return currentSpeed + (speedDiff > 0 ? maxChange : -maxChange);
    }
}

void updateMotorWithRamping(int motorPin, motorState &motor, int targetSpeed, bool targetDir)
{
    unsigned long currentTime = millis();

    // Update target values
    motor.targetSpeed = targetSpeed;
    motor.targetDir = targetDir;

    // Check if direction change requires stopping first
    if (motor.currentDir != targetDir && motor.currentSpeed > 0)
    {
        // Must stop before changing direction
        motor.targetSpeed = 0;
    }

    // Calculate ramped speed
    bool isEmergencyStop = emergencyStopActive || connectionLost;
    bool isStartup = (motor.currentSpeed < STARTUP_THRESHOLD && targetSpeed > 0);

    int newSpeed = calculateRampedSpeed(motor.targetSpeed, motor.currentSpeed, isEmergencyStop, isStartup);

    // Update motor state
    if (newSpeed == 0)
    {
        motor.currentDir = targetDir; // Safe to change direction when stopped
    }

    // Only update PWM if change is significant
    if (abs(newSpeed - motor.currentSpeed) >= MINIMUM_UPDATE_THRESHOLD || newSpeed == 0)
    {
        motor.currentSpeed = newSpeed;
        motor.lastUpdate = currentTime;
        ledcWrite(motorPin, motor.currentSpeed);
    }
}

void update()
{
    // Clear and logical enable/disable logic
    // Start with motors enabled, then check each disable condition
    newdroid.driveEN = 0; // Start with motors enabled

    // Check each disable condition individually
    if (emergencyStopActive)
    {
        newdroid.driveEN = 1; // Disable for emergency stop
        newdroid.left = 0;
        newdroid.right = 0;
#ifdef debug
        static bool emergencyPrinted = false;
        if (!emergencyPrinted)
        {
            Serial.println("Motors DISABLED: Emergency stop active");
            emergencyPrinted = true;
        }
#endif
    }
    else if (connectionLost)
    {
        newdroid.driveEN = 1; // Disable for connection loss
        newdroid.left = 0;
        newdroid.right = 0;
#ifdef debug
        static bool connectionPrinted = false;
        if (!connectionPrinted)
        {
            Serial.println("Motors DISABLED: Connection lost");
            connectionPrinted = true;
        }
#endif
    }
    else if (!newdroid.motorsEngaged)
    {
        newdroid.driveEN = 1; // Disable when Z button not held
        newdroid.left = 0;
        newdroid.right = 0;
#ifdef debug
        static bool zButtonPrinted = false;
        if (!zButtonPrinted)
        {
            Serial.println("Motors DISABLED: Z button not held");
            zButtonPrinted = true;
        }
#endif
    }
    else
    {
        // Z button held, no emergency, connection OK
        // Motors should be enabled regardless of speed values
        newdroid.driveEN = 0; // Enable motors

#ifdef debug
        // Reset the printed flags when motors are enabled
        static bool emergencyPrinted = false;
        static bool connectionPrinted = false;
        static bool zButtonPrinted = false;
        emergencyPrinted = false;
        connectionPrinted = false;
        zButtonPrinted = false;
#endif
    }

    // Update motors with advanced ramping
    updateMotorWithRamping(PWMLeft, leftMotor, newdroid.left, newdroid.leftdir);
    updateMotorWithRamping(PWMRight, rightMotor, newdroid.right, newdroid.rightdir);

    // CONTROL THE MOTOR HARDWARE
    // Set direction pins
    digitalWrite(motorLeftDir, newdroid.leftdir);   // 0 = forward, 1 = reverse
    digitalWrite(motorRightDir, newdroid.rightdir); // 0 = forward, 1 = reverse

    // Set enable pin (0 = disabled, 1 = enabled)
    // Note: driveEN uses inverted logic (1 = disable), so we invert it
    digitalWrite(motorEnable, !newdroid.driveEN);

// Debug motor enable state
#ifdef debug
    static bool lastEnableState = true; // Start with different state to force first print
    if (newdroid.driveEN != lastEnableState)
    {
        Serial.print("Motor Enable State Changed: ");
        Serial.print(newdroid.driveEN ? "DISABLED" : "ENABLED");
        Serial.print(" (Pin: ");
        Serial.print(!newdroid.driveEN ? "HIGH" : "LOW");
        Serial.print(", EngagedZ: ");
        Serial.print(newdroid.motorsEngaged);
        Serial.print(", EStop: ");
        Serial.print(emergencyStopActive);
        Serial.print(", ConnLost: ");
        Serial.print(connectionLost);
        Serial.println(")");
        lastEnableState = newdroid.driveEN;
    }
#endif

    // Print status changes for important events
    if (droid.motorsEngaged != newdroid.motorsEngaged)
    {
        if (newdroid.motorsEngaged && !newdroid.emergencyStop && !emergencyStopActive)
        {
            Serial.println("Motors ENGAGED (Z button held)");
        }
        else
        {
            Serial.println("Motors DISENGAGED");
        }
    }
}

void printStatus()
{
    Serial.print("Nunchuk - X: ");
    Serial.print(nunchuk_joystickX());
    Serial.print(", Y: ");
    Serial.print(nunchuk_joystickY());
    Serial.print(", C: ");
    Serial.print(nunchuk_buttonC());
    Serial.print(", Z: ");
    Serial.print(nunchuk_buttonZ());
    Serial.print(" | Motors - Left: ");
    Serial.print(leftMotor.currentSpeed);
    Serial.print("/");
    Serial.print(newdroid.left);
    Serial.print(" (dir: ");
    Serial.print(newdroid.leftdir);
    Serial.print("), Right: ");
    Serial.print(rightMotor.currentSpeed);
    Serial.print("/");
    Serial.print(newdroid.right);
    Serial.print(" (dir: ");
    Serial.print(newdroid.rightdir);
    Serial.print("), EN: ");
    Serial.print(newdroid.driveEN);
    Serial.print(", Engaged: ");
    Serial.print(newdroid.motorsEngaged);
    Serial.print(", EStop: ");
    Serial.print(emergencyStopActive);
    Serial.print(", Connected: ");
    Serial.println(!connectionLost);
}

int getMagnitude(int cx, int cy)
{
    // More efficient magnitude calculation without float conversion
    long x = cx;
    long y = cy;
    return (int)sqrt(x * x + y * y);
}