/*******************************************************************************************
 * Expert Tool ICS — FULLY ANNOTATED SOURCE (line-by-line comments)
 * -----------------------------------------------------------------------------------------
 * IMPORTANT: The logic and structure are IDENTICAL to the working app. Only comments are added.
 * The goal is to make every line understandable for a new engineer picking up this project.
 *******************************************************************************************/

 #include <furi.h>                               // Core Flipper RTOS API: threads, timers, records, etc.
 #include <furi_hal.h>                           // Hardware Abstraction Layer: GPIO, PWM, power, etc.
 #include <gui/gui.h>                            // GUI module: global GUI record and layers
 #include <gui/view_port.h>                      // ViewPort: drawable/input-enabled UI surface
 #include <gui/canvas.h>                         // Canvas drawing primitives (text, lines, boxes, etc.)
 #include <input/input.h>                        // Input events (keys, types)
 #include <notification/notification.h>          // Notification service: control LEDs, vibration, sound
 #include <notification/notification_messages.h> // Predefined LED sequences (e.g., set/reset RGB)
 #include <dialogs/dialogs.h>                    // Modal dialogs (confirmations, messages)
 #include <stdbool.h>                            // C99 bool, true/false
 #include <stdio.h>                              // snprintf() for small string formatting
 
 /*** PWM wiring (Flipper external header):
  *  + signal: PA7 (external pin "2 (A7)")
  *  - GND:    pin "8 (GND)")
  * Notes: PA7 is controlled as a PWM output; GND must be common with the inverter.
 ***/
 static const GpioPin* PWM_PIN = &gpio_ext_pa7;  // HAL descriptor for the external header pin PA7
 
 /* ---------- Geometry / constants (UI layout) ---------- */
 enum {                                           // Anonymous enum to group fixed layout constants
     CANVAS_W        = 128,                      // Screen width (pixels)
     CANVAS_H        = 64,                       // Screen height (pixels)
 
     TITLE_Y         = 14,                       // Baseline Y for the big title font
     ROW_Y0          = 26,                       // Baseline Y of the first list row
     ROW_DY          = 12,                       // Vertical spacing between list rows
 
     SCROLLBAR_X     = 124,                      // X position for right-side dotted scrollbar rail
     SCROLLBAR_W     = 3,                        // Width of the scrollbar thumb in pixels
     SCROLLBAR_Y0    = 2,                        // Top Y of the scrollbar rail
     SCROLLBAR_Y1    = 62,                       // Bottom Y of the scrollbar rail
 
     TIMER_MARGIN    = 6,                        // Gap between right-aligned timer text and rail
 };
 
 /* ---------- Safe GPIO helpers ---------- */
 static inline void pin_to_hiz(void) {           // Put PA7 in high-impedance input (safe, disconnected)
     furi_hal_gpio_init(                         // Configure a GPIO pin
         PWM_PIN,                                // -> target pin (PA7 on external header)
         GpioModeInput,                          // -> input mode (no driving)
         GpioPullNo,                             // -> no internal pull-up/down to avoid bias
         GpioSpeedLow);                          // -> speed not relevant for input; keep minimal
 }
 static inline void pin_to_pp_low(void) {        // Drive PA7 LOW actively (safe known level)
     furi_hal_gpio_init(                         // Configure output mode
         PWM_PIN,                                // -> pin PA7
         GpioModeOutputPushPull,                 // -> push-pull output (actively drives high/low)
         GpioPullNo,                             // -> no pull resistors
         GpioSpeedVeryHigh);                     // -> fast slew (harmless; keeps timing crisp)
     furi_hal_gpio_write(PWM_PIN, false);        // Write logic 0 (LOW) to the pin
 }
 
 /* ---------- Hardware PWM on PA7 ---------- */
 #define PWM_CH FuriHalPwmOutputIdTim1PA7        // HAL PWM channel identifier mapped to PA7 (TIM1)
 
 /* Stop PWM safely if currently running; update flag */
 static inline void pwm_hw_stop_safe(bool* running) {
     if(running && *running) {                   // Only stop if the caller tracks it as running
         furi_hal_pwm_stop(PWM_CH);              // HAL: stop the PWM unit on this channel
         furi_delay_ms(1);                       // Small delay to ensure hardware settles
         *running = false;                       // Reflect stopped state in caller-owned flag
     }
 }
 
 /* Start PWM at freq_hz with 50% duty; update flag if provided */
 static inline void pwm_hw_start_safe(uint32_t freq_hz, bool* running) {
     furi_hal_pwm_start(PWM_CH, freq_hz, 50);    // HAL: start PWM (freq in Hz, 50% duty cycle)
     if(running) *running = true;                // Mark as running if a flag pointer was passed
 }
 
 /* ---------- 5V helpers (OTG boost control) ---------- */
 /* "InverterId" identifies which inverter profile is active (affects 5V boost on Samsung) */
 typedef enum {
     InvEmbraco = 0,                             // Embraco inverter family
     InvSamsung = 1,                             // Samsung inverter family (requires OTG 5V)
 } InverterId;
 
 /* Enable/disable 5V only for Samsung (no-op for Embraco) */
 static inline void inverter_power_5v(InverterId inv, bool on){
     if(inv == InvSamsung){                      // Only Samsung uses OTG 5V boost
         if(on)  furi_hal_power_enable_otg();    // HAL: request USB-OTG 5V rail ON
         else    furi_hal_power_disable_otg();   // HAL: request USB-OTG 5V rail OFF
     }
 }
 
 /* Universal 5V switch used for safety cleanup (mostly turning it OFF) */
 static inline void power_5v_set(bool on){
     if(on)  furi_hal_power_enable_otg();        // Force OTG 5V ON
     else    furi_hal_power_disable_otg();       // Force OTG 5V OFF
 }
 
 /* ---------- Powered modes table ---------- */
 typedef struct {
     const char* name;                           // Row label to display
     uint32_t    freq_hz;                        // PWM frequency (0 => Stand by = no PWM)
     uint8_t     led_blink_hz;                   // LED blink frequency (0 => LED off)
     uint32_t    default_secs;                   // Auto-off seconds (if limit_runtime == true)
 } Mode;
 
 static const Mode kModes[] = {                  // Mode list (indices used elsewhere; do not reorder)
     {"Stand by", 0,   0,   0},                  // 0: No PWM, pin forced LOW, no timer
     {"Low speed", 55, 1, 120},                  // 1: 55 Hz PWM, LED 1 Hz, 2 minutes limit
     {"Mid speed", 100,2,  60},                  // 2: 100 Hz PWM, LED 2 Hz, 1 minute limit
     {"Max speed", 160,4,  30},                  // 3: 160 Hz PWM, LED 4 Hz, 30 seconds limit
 };
 #define MODE_COUNT (sizeof(kModes)/sizeof(kModes[0])) // Compute number of entries at compile-time
 
 /* ---------- Help text (per inverter) ---------- */
 static const char* HELP_EMBRACO[] = {           // Embraco help lines (scrollable plain strings)
     "Connect wires as follows:",
     "",
     "2 (A7)    -> inverter +",
     "(usually RED wire)",
     "8 (GND)  -> inverter -",
     "(usually WHITE wire)",
     "",
     "Note:",
     "This app provides",
     "3 test speeds:",
     "",
     "Low speed:",
     "2000 RPM (VNE)",
     "1800 RPM (VEG, FMF)",
     "",
     "Mid speed:",
     "3000 RPM",
     "(VNE, VEG, FMF)",
     "",
     "Max speed:",
     "4500 RPM",
     "(VNE, VEG, FMF)",
     "",
     "Embraco compressors",
     "support many speeds",
     "with 30 RPM steps.",
     "",
     "----------------",
     "",
     "App created by",
     "Adam Gray",
     "Founder of",
     "Expert Hub",
     "experthub.app",
     "",
     "----------------",
     "",
     "Press BACK to start.",
 };
 #define HELP_EMBRACO_COUNT (sizeof(HELP_EMBRACO)/sizeof(HELP_EMBRACO[0])) // Number of Embraco lines
 
 static const char* HELP_SAMSUNG[] = {           // Samsung help placeholder
     "In development",
 };
 #define HELP_SAMSUNG_COUNT (sizeof(HELP_SAMSUNG)/sizeof(HELP_SAMSUNG[0])) // Number of Samsung lines
 
 /* ---------- Screens (state machine) ---------- */
 typedef enum {
     ScreenSelectInverter = 0,                   // First screen: pick Embraco or Samsung
     ScreenMenu,                                 // Main menu (safe or powered variant)
     ScreenHelp,                                 // Scrollable help view
     ScreenSettings,                             // Settings screen (toggles + inverter selection)
 } ScreenId;
 
 /* ---------- Application runtime state ---------- */
 typedef struct {
     ScreenId screen;                            // Current screen
     InverterId inverter;                        // Selected inverter profile
     bool powered;                               // false => SAFE menu; true => POWERED menu
 
     uint8_t cursor;                             // Selected row index within the visible window
     uint8_t first_visible;                      // Top row index in the 4-row window
     uint8_t active;                             // Active powered mode (0..MODE_COUNT-1)
 
     uint8_t help_top_line;                      // Scroll offset for help text (top visible line)
 
     bool limit_runtime;                         // If true, enforce per-mode timeout
     bool arrow_captcha;                         // Placeholder toggle (UI only)
 
     NotificationApp* notif;                     // Notification (LED) service handle
     FuriTimer* led_timer;                       // LED blink timer (periodic)
     bool led_on;                                // Current LED state (toggled by timer)
 
     bool pwm_running;                           // Tracks whether PWM is currently running
 
     bool hint_visible;                          // If true, draw the bottom hint ribbon
     FuriTimer* hint_timer;                      // One-shot timer to auto-hide the hint
 
     FuriTimer* tick_timer;                      // 1 Hz countdown tick timer
     FuriTimer* off_timer;                       // One-shot precise auto-off timer
     uint32_t remaining_ms;                      // Remaining milliseconds for countdown
     bool timeout_expired;                       // Set by off_timer; consumed in main loop
 
     Gui* gui;                                   // Global GUI record (owner)
     ViewPort* vp;                               // ViewPort object attached to GUI
     FuriMessageQueue* q;                        // Input event queue for main loop
 } AppState;
 
 /* ---------- LED helpers ---------- */
 static void led_set(NotificationApp* n, bool on){ // Drive LED using Notification sequences
     if(!n) return;                               // Guard: notification service may be NULL
     if(on) notification_message(n, &sequence_set_green_255); // Set green LED full brightness
     else   notification_message(n, &sequence_reset_rgb);     // Reset LEDs (off)
 }
 static void led_timer_cb(void* ctx){             // Called periodically to toggle LED state
     AppState* s = ctx;                           // Recover app state pointer from timer context
     s->led_on = !s->led_on;                      // Flip LED boolean
     led_set(s->notif, s->led_on);                // Apply new LED state
 }
 static void led_apply(AppState* s, uint8_t blink_hz){ // Start/stop LED blinking according to mode
     if(s->led_timer){                            // If a previous timer exists
         furi_timer_stop(s->led_timer);           // -> stop it
         furi_timer_free(s->led_timer);           // -> free it
         s->led_timer = NULL;                     // -> clear pointer
     }
     s->led_on = false;                           // Reset LED state
     led_set(s->notif, false);                    // Turn LED off
 
     if(blink_hz == 0) return;                    // No blink requested: done
 
     uint32_t ms = 1000U / (blink_hz * 2U);       // Toggle period = half cycle (on/off)
     if(ms == 0) ms = 1;                          // Safety clamp against division rounding
     s->led_timer = furi_timer_alloc(             // Create a periodic timer
         led_timer_cb,                            // -> callback to toggle LED
         FuriTimerTypePeriodic,                   // -> periodic mode
         s);                                      // -> context back to our AppState
     furi_timer_start(s->led_timer, furi_ms_to_ticks(ms)); // Arm timer with computed period
 }
 
 /* ---------- Dotted scrollbar renderer ---------- */
 static void draw_scrollbar_dotted(Canvas* c, uint16_t total_steps, uint16_t pos){
     if(total_steps <= 1) return;                 // Skip if no need for a scrollbar
 
     const uint16_t x  = SCROLLBAR_X;             // Rail X coordinate
     const uint16_t y0 = SCROLLBAR_Y0;            // Rail top
     const uint16_t y1 = SCROLLBAR_Y1;            // Rail bottom
 
     for(uint16_t y = y0; y <= y1; y += 3){       // Draw dotted rail (every 3 px)
         canvas_draw_dot(c, x, y);                // Single pixel dot
     }
 
     const uint16_t track_h = (uint16_t)(y1 - y0);// Rail height
     uint16_t denom = (total_steps > 1) ? (uint16_t)(total_steps - 1) : 1; // Avoid /0
     uint16_t thumb_y = (uint16_t)(y0 + (pos * track_h) / denom);          // Map pos to Y
 
     if(thumb_y > (uint16_t)(y1 - 1)) thumb_y = (uint16_t)(y1 - 1);        // Clamp bottom
     if(thumb_y < y0) thumb_y = y0;                                        // Clamp top
 
     canvas_draw_box(c, (uint16_t)(x - 1), (uint16_t)(thumb_y - 1), SCROLLBAR_W, 4); // Thumb
 }
 
 /* ---------- Checkmark glyph ---------- */
 static void draw_checkmark(Canvas* c, int x, int baseline_y){
     int y = baseline_y - 6;                      // Position the 7x7 check slightly above baseline
     canvas_draw_line(c, x,     y+3, x+2, y+5);   // First segment (lower left to mid)
     canvas_draw_line(c, x+2,   y+5, x+7, y   );  // Second segment (mid to upper right)
 }
 
 /* ---------- Countdown / auto-off timers ---------- */
 static void tick_timer_cb(void* ctx){            // 1 Hz tick to update remaining_ms and redraw
     AppState* s = ctx;                           // Recover state
     if(s->remaining_ms >= 1000) s->remaining_ms -= 1000; // Subtract 1 second if possible
     else s->remaining_ms = 0;                    // Clamp at 0
     if(s->vp) view_port_update(s->vp);           // Trigger redraw so title timer updates
 }
 static void off_timer_cb(void* ctx){             // One-shot expiry handler
     AppState* s = ctx;                           // Recover state
     s->remaining_ms = 0;                         // Ensure timer shows as zero
     s->timeout_expired = true;                   // Signal main loop to switch to Stand by
     if(s->vp) view_port_update(s->vp);           // Ask GUI to refresh immediately
 }
 static void stop_timers(AppState* s){            // Stop both countdown timers if active
     if(s->tick_timer) furi_timer_stop(s->tick_timer); // Stop tick timer (keeps allocated)
     if(s->off_timer)  furi_timer_stop(s->off_timer);  // Stop one-shot timer
 }
 static void free_timers(AppState* s){            // Free both countdown timers and clear pointers
     if(s->tick_timer){ furi_timer_free(s->tick_timer); s->tick_timer = NULL; }
     if(s->off_timer){  furi_timer_free(s->off_timer);  s->off_timer  = NULL; }
 }
 static void start_tick_timer_if_needed(AppState* s){ // Arm timers depending on current mode/state
     stop_timers(s);                               // Ensure no older timers are ticking
     s->remaining_ms = 0;                          // Reset remaining time
     s->timeout_expired = false;                   // Clear any pending timeout flag
 
     if(!s->powered) return;                       // Only powered menu uses runtime limit
     if(!s->limit_runtime) return;                 // If limit disabled, no timers
     if(s->active == 0) return;                    // Stand by (index 0) never has a timer
 
     uint32_t secs = kModes[s->active].default_secs; // Pull per-mode default seconds
     if(secs == 0) return;                         // 0 means unlimited: no timers
 
     s->remaining_ms = secs * 1000U;               // Convert seconds -> milliseconds
 
     if(!s->tick_timer) s->tick_timer =           // Lazy allocate tick timer if needed
         furi_timer_alloc(tick_timer_cb, FuriTimerTypePeriodic, s);
     if(!s->off_timer)  s->off_timer  =           // Lazy allocate off timer if needed
         furi_timer_alloc(off_timer_cb,  FuriTimerTypeOnce,     s);
 
     furi_timer_start(s->tick_timer, furi_ms_to_ticks(1000));       // Start 1 Hz tick
     furi_timer_start(s->off_timer,  furi_ms_to_ticks(s->remaining_ms)); // Start one-shot
 }
 
 /* ---------- Mode application (Stand by / Low / Mid / Max) ---------- */
 static void apply_mode(AppState* s, uint8_t idx){
     if(idx >= MODE_COUNT) return;                // Guard invalid indices
     s->active = idx;                             // Remember which powered mode is active
 
     const Mode* m = &kModes[idx];                // Pointer to chosen mode descriptor
 
     if(m->freq_hz == 0){                         // Stand by: special no-PWM mode
         pwm_hw_stop_safe(&s->pwm_running);       // Ensure PWM is off
         pin_to_pp_low();                         // Actively pull output LOW (safe)
         stop_timers(s);                          // Cancel any countdown
         s->remaining_ms = 0;                     // Reset countdown remaining
         s->timeout_expired = false;              // Clear timeout event flag
     } else {                                     // Any PWM-enabled mode
         pwm_hw_stop_safe(&s->pwm_running);       // Stop previous PWM (if any)
         pwm_hw_start_safe(m->freq_hz, &s->pwm_running); // Start new PWM at target frequency
         start_tick_timer_if_needed(s);           // (Re)arm limit timers if configured
     }
     led_apply(s, m->led_blink_hz);               // Update LED blink to reflect activity level
 }
 
 /* ---------- Hint timer (short BACK overlay) ---------- */
 static void hint_timer_cb(void* ctx){            // Hides the hint after a short delay
     AppState* s = ctx;                           // Recover state
     s->hint_visible = false;                     // Turn off bottom hint ribbon
     if(s->vp) view_port_update(s->vp);           // Redraw to remove it from the screen
 }
 
 /* ---------- Blocking alerts (confirmations) ---------- */
 static bool show_limit_alert_confirm(void){      // Warn when disabling runtime limit
     DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS); // Open Dialogs service
     DialogMessage* msg = dialog_message_alloc();            // Create a dialog object
 
     dialog_message_set_header(                   // Configure title “Alert”
         msg, "Alert", 64, 2, AlignCenter, AlignTop);
     dialog_message_set_text(                     // Body text (3 lines), left-aligned
         msg,
         "Long run without condenser\n"
         "and evaporator fans may\n"
         "damage compressor parts.",
         6, 16, AlignLeft, AlignTop);
     dialog_message_set_buttons(msg, "Cancel", NULL, "Confirm"); // Left/Right buttons
 
     DialogMessageButton res = dialog_message_show(dialogs, msg); // Show dialog and wait result
 
     dialog_message_free(msg);                    // Free dialog object
     furi_record_close(RECORD_DIALOGS);           // Close Dialogs service
     return (res == DialogMessageButtonRight);    // True only if “Confirm” was pressed
 }
 
 static bool show_power_on_confirm(void){         // Warn before enabling outputs/5V
     DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS); // Open Dialogs service
     DialogMessage* msg = dialog_message_alloc();            // Create a dialog object
 
     dialog_message_set_header(                   // Configure title “Alert”
         msg, "Alert", 64, 2, AlignCenter, AlignTop);
     dialog_message_set_text(                     // Body text centered
         msg,
         "Check your wiring!\n"
         "All pins will be activated!\n"
         "Check help!",
         64, 16, AlignCenter, AlignTop);
     dialog_message_set_buttons(msg, "Cancel", NULL, "Confirm"); // Left/Right buttons
 
     DialogMessageButton res = dialog_message_show(dialogs, msg); // Show and wait
 
     dialog_message_free(msg);                    // Free
     furi_record_close(RECORD_DIALOGS);           // Close
     return (res == DialogMessageButtonRight);    // True if “Confirm” pressed
 }
 
 /* ---------- Help layout math ---------- */
 static inline void help_layout_params(           // Compute visible help lines & max scroll
     uint8_t total_lines,                         // -> number of lines in help text
     uint8_t* out_max_lines,                      // <- how many lines fit on screen
     uint8_t* out_max_top_line){                  // <- largest top-line index
     const uint8_t top = 10;                      // Top margin for help text
     const uint8_t line_h = 9;                    // Line height for FontSecondary
     uint8_t ml = (uint8_t)((CANVAS_H - top) / line_h); // Integer lines that fit
     if(ml < 1) ml = 1;                           // At least one line
     uint8_t mtl = (total_lines > ml) ? (uint8_t)(total_lines - ml) : 0; // Max scroll offset
     if(out_max_lines) *out_max_lines = ml;       // Return lines-on-screen
     if(out_max_top_line) *out_max_top_line = mtl;// Return max top-line index
 }
 
 /* ---------- Title drawing ---------- */
 static void draw_title(Canvas* c, const AppState* s){ // Draws "<Inverter> Starter" + countdown
     canvas_set_font(c, FontPrimary);            // Big font
     canvas_set_color(c, ColorBlack);            // Black pixels on white background
 
     const char* inv_name =                      // Choose inverter name for title
         (s->inverter == InvEmbraco) ? "Embraco" : "Samsung";
     char title[32];                             // Small stack buffer for formatting title
     snprintf(title, sizeof(title), "%s Starter", inv_name); // Compose "X Starter"
     canvas_draw_str(c, 4, TITLE_Y, title);      // Render at left padding x=4
 
     if(s->remaining_ms > 0){                    // If a countdown is active, show “NNs” on the right
         char tbuf[16];                          // Buffer for seconds string
         unsigned long sec =                     // Round up milliseconds to next second
             (unsigned long)((s->remaining_ms + 999)/1000);
         snprintf(tbuf, sizeof(tbuf), "%lus", sec);      // Format as "NNs"
         uint16_t w = canvas_string_width(c, tbuf);      // Measure text width
         uint16_t right_x = (uint16_t)(SCROLLBAR_X - TIMER_MARGIN); // Right bound
         uint16_t x = (w <= right_x) ? (uint16_t)(right_x - w) : 2; // Right align or clamp
         canvas_draw_str(c, x, TITLE_Y, tbuf);   // Draw the timer text
     }
 }
 
 /* ---------- Select Inverter screen ---------- */
 static void draw_select_inverter(Canvas* c, const AppState* s){
     canvas_clear(c);                            // Clear full canvas
     canvas_set_color(c, ColorBlack);            // Black drawing color
 
     canvas_set_font(c, FontPrimary);            // Title font
     canvas_draw_str(c, 4, TITLE_Y, "Inverter type"); // Draw title
 
     canvas_set_font(c, FontSecondary);          // Row font
     int y = ROW_Y0;                             // Start at first row baseline
     canvas_draw_str(c, 2, y, (s->cursor == 0) ? ">" : " "); // Caret for Embraco
     canvas_draw_str(c, 14, y, "Embraco");       // Row text for Embraco
     y += ROW_DY;                                // Move to second row baseline
     canvas_draw_str(c, 2, y, (s->cursor == 1) ? ">" : " "); // Caret for Samsung
     canvas_draw_str(c, 14, y, "Samsung");       // Row text for Samsung
 
     draw_scrollbar_dotted(c, 2, s->cursor);     // Scrollbar with 2 steps at current cursor
 
     if(s->hint_visible){                        // If hint should be shown…
         const char* msg = "Long press back to exit"; // Footer message
         uint16_t text_h = 10;                   // Footer height (approx)
         uint16_t text_y = (uint16_t)(CANVAS_H - 2); // Baseline near bottom
         canvas_set_color(c, ColorBlack);        // Switch to black for background
         canvas_draw_box(c, 0, (uint16_t)(text_y - text_h), CANVAS_W, (uint16_t)(text_h + 4)); // Ribbon
         canvas_set_color(c, ColorWhite);        // White text
         canvas_draw_str(c, 14, text_y, msg);    // Draw footer text inset
         canvas_set_color(c, ColorBlack);        // Restore black for future drawing
     }
 }
 
 /* ---------- Main Menu screen (safe or powered) ---------- */
 static void draw_menu(Canvas* c, const AppState* s){
     canvas_clear(c);                            // Clear the screen
     draw_title(c, s);                           // Draw title and possible countdown
 
     canvas_set_font(c, FontSecondary);          // List font
     const uint8_t MAX_ROWS = 4;                 // We always display up to 4 rows
 
     const bool powered = s->powered;            // Snapshot powered flag
     uint8_t row_total = powered                 // Total number of rows depends on powered state
         ? (uint8_t)(MODE_COUNT + 3)             // Powered: 0..3 modes, 4 off, 5 settings, 6 help
         : 3;                                    // Safe: 0 on, 1 settings, 2 help
 
     uint8_t first_visible = s->first_visible;   // Clamp top-of-window index
     if(first_visible + MAX_ROWS > row_total){
         first_visible = (row_total > MAX_ROWS) ? (uint8_t)(row_total - MAX_ROWS) : 0;
     }
 
     for(uint8_t i = 0; i < MAX_ROWS; i++){      // Draw up to 4 rows
         uint8_t row = (uint8_t)(first_visible + i); // Actual row index in the list
         if(row >= row_total) break;             // Stop if out of range
         int y = ROW_Y0 + i*ROW_DY;              // Compute baseline Y for this row
 
         if(row == s->cursor) canvas_draw_str(c, 2, y, ">");
         else canvas_draw_str(c, 2, y, " ");     // Draw caret or space at left
 
         if(powered){                            // Powered menu contents
             if(row < MODE_COUNT){               // One of the 4 modes
                 canvas_draw_str(c, 14, y, kModes[row].name); // Draw mode name
                 if(row == s->active){           // If this is the active mode, show checkmark
                     int check_x = (int)SCROLLBAR_X - TIMER_MARGIN - 10; // Right area
                     if(check_x < 90) check_x = 90; // Keep away from regular text
                     draw_checkmark(c, check_x, y); // Paint check
                 }
             } else if(row == MODE_COUNT){       // Power off entry
                 canvas_draw_str(c, 14, y, "Power off");
             } else if(row == MODE_COUNT + 1){   // Settings
                 canvas_draw_str(c, 14, y, "Settings");
             } else {                            // Help
                 canvas_draw_str(c, 14, y, "Help");
             }
         } else {                                // Safe (unpowered) menu contents
             if(row == 0) canvas_draw_str(c, 14, y, "Power on");
             else if(row == 1) canvas_draw_str(c, 14, y, "Settings");
             else canvas_draw_str(c, 14, y, "Help");
         }
     }
 
     draw_scrollbar_dotted(c, row_total, s->cursor); // Draw right-side scrollbar
 
     if(s->hint_visible){                        // Optional bottom hint ribbon
         const char* msg = "Long press back to exit";
         uint16_t text_h = 10;
         uint16_t text_y = (uint16_t)(CANVAS_H - 2);
         canvas_set_color(c, ColorBlack);
         canvas_draw_box(c, 0, (uint16_t)(text_y - text_h), CANVAS_W, (uint16_t)(text_h + 4));
         canvas_set_color(c, ColorWhite);
         canvas_draw_str(c, 14, text_y, msg);
         canvas_set_color(c, ColorBlack);
     }
 }
 
 /* ---------- Help screen ---------- */
 static void draw_help(Canvas* c, const AppState* s){
     canvas_clear(c);                            // Clear the display
     canvas_set_font(c, FontSecondary);          // Use smaller font for content
     canvas_set_color(c, ColorBlack);            // Draw in black
 
     const char* const* LINES =                  // Choose which help text to show
         (s->inverter == InvEmbraco) ? HELP_EMBRACO : HELP_SAMSUNG;
     const uint8_t LINES_COUNT =                 // And how many lines it has
         (s->inverter == InvEmbraco) ? HELP_EMBRACO_COUNT : HELP_SAMSUNG_COUNT;
 
     uint8_t max_lines, max_top_line;            // Compute visible capacity & max scroll
     help_layout_params(LINES_COUNT, &max_lines, &max_top_line);
 
     const uint8_t top = 10;                     // Top margin where to start drawing text
     const uint8_t line_h = 9;                   // Line height for this font
 
     for(uint8_t i=0;i<max_lines;i++){           // Draw visible window of lines
         uint8_t idx = (uint8_t)(s->help_top_line + i); // Line index to show
         if(idx >= LINES_COUNT) break;           // Stop when out of content
         canvas_draw_str(c, 2, (uint8_t)(top + i*line_h), LINES[idx]); // Draw line at computed Y
     }
 
     uint16_t total_steps = (uint16_t)(max_top_line + 1); // Steps for scrollbar (top positions)
     if(total_steps < 1) total_steps = 1;         // Ensure at least one step
     draw_scrollbar_dotted(c, total_steps, s->help_top_line); // Draw scrollbar based on scroll pos
 }
 
 /* ---------- Settings screen ---------- */
 static void draw_settings(Canvas* c, const AppState* s){
     canvas_clear(c);                            // Clear screen
 
     canvas_set_font(c, FontPrimary);            // Title font
     canvas_set_color(c, ColorBlack);            // Draw in black
     canvas_draw_str(c, 4, TITLE_Y, "Settings"); // Title text
 
     canvas_set_font(c, FontSecondary);          // Body font
 
     const uint8_t MAX_ROWS = 4;                 // Visible rows at once
     const uint8_t ROW_TOTAL = 5;                // Total rows including header
 
     uint8_t first_visible = s->first_visible;   // Clamp window against total rows
     if(first_visible + MAX_ROWS > ROW_TOTAL){
         first_visible = (ROW_TOTAL > MAX_ROWS) ? (uint8_t)(ROW_TOTAL - MAX_ROWS) : 0;
     }
 
     for(uint8_t i=0;i<MAX_ROWS;i++){            // Loop visible rows
         uint8_t row = (uint8_t)(first_visible + i); // Actual row index
         if(row >= ROW_TOTAL) break;             // Stop if past end
         int y = ROW_Y0 + i*ROW_DY;              // Baseline Y for this row
 
         if(row == 2){                           // Row 2 is a non-selectable header
             canvas_draw_str(c, 4, y, "Inverter type");
             continue;                           // Skip caret and value rendering
         }
 
         canvas_draw_str(c, 2, y, (s->cursor == row) ? ">" : " "); // Caret for selectable rows
 
         if(row == 0){                           // Limit runtime toggle row
             canvas_draw_str(c, 14, y, "Limit run time");
             const char* val = s->limit_runtime ? "Yes" : "No"; // Value text
             uint16_t w = canvas_string_width(c, val);          // Measure width for right align
             uint16_t right_x = (uint16_t)(SCROLLBAR_X - TIMER_MARGIN); // Right bound
             uint16_t x = (w <= right_x) ? (uint16_t)(right_x - w) : 2; // Right align/clamp
             canvas_draw_str(c, x, y, val);     // Draw value
         } else if(row == 1){                    // Arrow captcha toggle (placeholder)
             canvas_draw_str(c, 14, y, "Arrow captcha");
             const char* val = s->arrow_captcha ? "Yes" : "No";
             uint16_t w = canvas_string_width(c, val);
             uint16_t right_x = (uint16_t)(SCROLLBAR_X - TIMER_MARGIN);
             uint16_t x = (w <= right_x) ? (uint16_t)(right_x - w) : 2;
             canvas_draw_str(c, x, y, val);
         } else if(row == 3){                    // Radio: Embraco
             canvas_draw_str(c, 14, y, "Embraco");
             if(s->inverter == InvEmbraco){      // Show check on selected inverter
                 int check_x = (int)SCROLLBAR_X - TIMER_MARGIN - 10;
                 if(check_x < 90) check_x = 90;
                 draw_checkmark(c, check_x, y);
             }
         } else if(row == 4){                    // Radio: Samsung
             canvas_draw_str(c, 14, y, "Samsung");
             if(s->inverter == InvSamsung){
                 int check_x = (int)SCROLLBAR_X - TIMER_MARGIN - 10;
                 if(check_x < 90) check_x = 90;
                 draw_checkmark(c, check_x, y);
             }
         }
     }
 
     draw_scrollbar_dotted(c, ROW_TOTAL, s->cursor); // Right scrollbar
 }
 
 /* ---------- Draw dispatcher ---------- */
 static void draw_cb(Canvas* c, void* ctx){      // ViewPort draw callback
     AppState* s = ctx;                          // Cast context back to AppState
     switch(s->screen){                          // Dispatch based on current screen
         case ScreenSelectInverter: draw_select_inverter(c, s); break;
         case ScreenMenu:           draw_menu(c, s);            break;
         case ScreenHelp:           draw_help(c, s);            break;
         case ScreenSettings:       draw_settings(c, s);        break;
         default:                   draw_menu(c, s);            break; // Fallback
     }
 }
 
 /* ---------- Input queue plumbing ---------- */
 typedef struct { FuriMessageQueue* q; } InputCtx; // Wrapper to pass queue to callback
 static void vp_input_cb(InputEvent* e, void* ctx){ // ViewPort input callback (ISR-ish context)
     InputCtx* ic = ctx;                         // Recover wrapper
     InputEvent ev = *e;                         // Copy event (avoid pointer lifetime issues)
     furi_message_queue_put(ic->q, &ev, 0);      // Push event to queue (non-blocking)
 }
 
 /* ---------- State transitions for power ---------- */
 static void enter_safe_menu(AppState* s){       // Switch to SAFE menu (unpowered state)
     s->powered = false;                         // Mark as unpowered
     s->cursor = 0;                              // Reset selection to first row
     s->first_visible = 0;                       // Reset window offset to top
 
     pwm_hw_stop_safe(&s->pwm_running);          // Ensure PWM is stopped
     pin_to_hiz();                               // Disconnect PA7 (no driving)
     power_5v_set(false);                        // Force OTG 5V OFF universally
     led_apply(s, 0);                            // Turn LED off (no blinking)
     stop_timers(s);                             // Stop countdown timers if any
     s->remaining_ms = 0;                        // Clear countdown
     s->timeout_expired = false;                 // Clear timeout flag
 }
 
 static void enter_powered_menu_standby(AppState* s){ // Switch to POWERED menu, Stand by mode
     s->powered = true;                          // Mark as powered
     s->cursor = 0;                              // Place caret on "Stand by"
     s->first_visible = 0;                       // Reset window
     inverter_power_5v(s->inverter, true);       // Enable 5V only for Samsung (no-op otherwise)
     apply_mode(s, 0);                           // Apply Stand by: output LOW, no timers, LED off
 }
 
 /* ---------- Application entry point ---------- */
 int32_t expert_tool_ics(void* p){               // Main function called by app loader
     UNUSED(p);                                  // We don't use the incoming parameter
 
     AppState s = {                               // Initialize all state fields explicitly
         .screen = ScreenSelectInverter,         // Start on inverter selection screen
         .inverter = InvEmbraco,                 // Default selection (user can change)
         .powered = false,                       // Start in SAFE state
         .cursor = 0,                            // Start with first row selected
         .first_visible = 0,                     // Top of list window
         .active = 0,                            // Active powered mode index (Stand by)
         .help_top_line = 0,                     // Help scroller at top
         .limit_runtime = true,                  // Enforce per-mode timeouts by default
         .arrow_captcha = true,                  // Placeholder toggle default is Yes
         .notif = furi_record_open(RECORD_NOTIFICATION), // Acquire Notification service handle
         .led_timer = NULL,                      // No LED timer yet
         .led_on = false,                        // LED off initially
         .pwm_running = false,                   // PWM not running
         .hint_visible = false,                  // Hint ribbon hidden
         .hint_timer = NULL,                     // No hint timer
         .tick_timer = NULL,                     // No 1 Hz timer
         .off_timer = NULL,                      // No one-shot timer
         .remaining_ms = 0,                      // No countdown active
         .timeout_expired = false,               // No timeout pending
         .gui = NULL,                            // Will be set below
         .vp = NULL,                             // Will be set below
         .q = NULL,                              // Will be set below
     };
 
     s.gui = furi_record_open(RECORD_GUI);       // Acquire GUI service
     s.vp = view_port_alloc();                   // Create a ViewPort (draw+input)
     s.q  = furi_message_queue_alloc(8, sizeof(InputEvent)); // Create queue for input events
     InputCtx ic = {.q = s.q};                   // Wrap queue to pass into input callback
 
     view_port_draw_callback_set(s.vp, draw_cb, &s); // Attach draw callback with AppState context
     view_port_input_callback_set(s.vp, vp_input_cb, &ic); // Attach input callback with queue wrapper
     gui_add_view_port(s.gui, s.vp, GuiLayerFullscreen);   // Add viewport to full-screen GUI layer
 
     pin_to_hiz();                               // Absolute safety: disconnect PA7 at start
     power_5v_set(false);                        // Make sure OTG 5V is OFF at start
     led_apply(&s, 0);                           // Ensure LED is off (no blink)
 
     const uint8_t MAX_ROWS = 4;                 // Used for wrapping navigation (visible height)
     (void)MAX_ROWS;                             // Silence “unused variable” warnings if any
 
     bool exit_app = false;                      // Main loop termination flag
     InputEvent ev;                              // Local buffer for input events
 
     while(!exit_app){                           // Main event loop
         if(s.timeout_expired){                  // If one-shot auto-off timer fired…
             s.timeout_expired = false;          // -> clear flag
             enter_powered_menu_standby(&s);     // -> fall back to powered Stand by
             view_port_update(s.vp);             // -> request immediate redraw
         }
 
         if(furi_message_queue_get(s.q, &ev, 100) == FuriStatusOk){ // Wait up to 100ms for input
             if(ev.type == InputTypeLong && ev.key == InputKeyBack){ // Long BACK exits app
                 exit_app = true;                 // -> set termination flag
                 view_port_update(s.vp);          // -> repaint once more (optional)
                 continue;                        // -> go next loop iteration (will exit)
             }
 
             switch(s.screen){                    // Dispatch per-screen input logic
                 case ScreenSelectInverter: {     // Inverter selection screen
                     if(ev.type == InputTypeShort || ev.type == InputTypeRepeat){ // Short or held
                         if(ev.key == InputKeyUp){           // UP toggles between 0 and 1
                             s.cursor = (s.cursor == 0) ? 1 : 0;
                         } else if(ev.key == InputKeyDown){  // DOWN toggles between 1 and 0
                             s.cursor = (s.cursor == 1) ? 0 : 1;
                         } else if(ev.key == InputKeyOk){    // OK applies selection
                             s.inverter = (s.cursor == 0) ? InvEmbraco : InvSamsung; // Save choice
                             enter_safe_menu(&s);             // Jump into SAFE main menu
                             s.screen = ScreenMenu;           // Switch screen to Menu
                         } else if(ev.key == InputKeyBack){  // Short BACK shows hint ribbon
                             s.hint_visible = true;           // -> make ribbon visible
                             if(!s.hint_timer){               // -> allocate one-shot timer once
                                 s.hint_timer =
                                     furi_timer_alloc(hint_timer_cb, FuriTimerTypeOnce, &s);
                             }
                             furi_timer_start(                 // -> start ~1.5s hide timer
                                 s.hint_timer, furi_ms_to_ticks(1500));
                         }
                     }
                 } break;
 
                 case ScreenMenu: {               // Main menu (safe or powered)
                     const bool powered = s.powered;          // Snapshot
                     uint8_t row_total = powered              // Compute row count
                         ? (uint8_t)(MODE_COUNT + 3)
                         : 3;
 
                     if(ev.type == InputTypeShort){           // React to short presses only
                         if(ev.key == InputKeyUp){            // Move selection up (with wrap)
                             if(s.cursor == 0){
                                 s.cursor = (uint8_t)(row_total - 1); // Wrap to bottom
                                 s.first_visible =
                                     (row_total > MAX_ROWS) ? (uint8_t)(row_total - MAX_ROWS) : 0;
                             } else {
                                 s.cursor--;                  // Move up one
                                 if(s.cursor < s.first_visible) s.first_visible = s.cursor; // Scroll up
                             }
                         } else if(ev.key == InputKeyDown){   // Move selection down (with wrap)
                             if(s.cursor == (uint8_t)(row_total - 1)){
                                 s.cursor = 0;                // Wrap to top
                                 s.first_visible = 0;         // Reset window
                             } else {
                                 s.cursor++;                  // Move down one
                                 if(s.cursor >= s.first_visible + MAX_ROWS){ // Scroll window down
                                     s.first_visible = (uint8_t)(s.cursor - (MAX_ROWS - 1));
                                 }
                             }
                         } else if(ev.key == InputKeyOk){     // Activate selected item
                             if(powered){
                                 if(s.cursor < MODE_COUNT){   // One of the powered modes
                                     apply_mode(&s, s.cursor);// -> run that mode
                                 } else if(s.cursor == MODE_COUNT){ // "Power off"
                                     enter_safe_menu(&s);     // -> go to SAFE (unpowered)
                                 } else if(s.cursor == MODE_COUNT + 1){ // "Settings"
                                     s.screen = ScreenSettings; // -> settings screen
                                     s.cursor = 0;            // -> reset cursor
                                     s.first_visible = 0;     // -> reset window
                                 } else {                     // "Help"
                                     enter_safe_menu(&s);     // -> ensure safe state
                                     s.screen = ScreenHelp;   // -> open help screen
                                     s.help_top_line = 0;     // -> scroll to top
                                 }
                             } else {                          // SAFE menu actions
                                 if(s.cursor == 0){            // "Power on"
                                     if(show_power_on_confirm()){ // -> confirm safety alert
                                         enter_powered_menu_standby(&s); // -> powered Stand by
                                     }
                                 } else if(s.cursor == 1){     // "Settings"
                                     s.screen = ScreenSettings;// -> go to settings
                                     s.cursor = 0;             // -> reset cursor
                                     s.first_visible = 0;      // -> reset window
                                 } else {                      // "Help"
                                     s.screen = ScreenHelp;    // -> help screen
                                     s.help_top_line = 0;      // -> scroll to top
                                 }
                             }
                         } else if(ev.key == InputKeyBack){   // Short BACK => show hint ribbon
                             s.hint_visible = true;            // -> show
                             if(!s.hint_timer){                // -> allocate one-shot timer if needed
                                 s.hint_timer =
                                     furi_timer_alloc(hint_timer_cb, FuriTimerTypeOnce, &s);
                             }
                             furi_timer_start(                  // -> arm auto-hide after ~1.5s
                                 s.hint_timer, furi_ms_to_ticks(1500));
                         }
                     }
                 } break;
 
                 case ScreenHelp: {              // Help view with vertical scrolling
                     if(ev.type == InputTypeShort || ev.type == InputTypeRepeat){
                         const uint8_t total_lines =         // Determine content length by inverter
                             (s.inverter == InvEmbraco) ? HELP_EMBRACO_COUNT : HELP_SAMSUNG_COUNT;
                         uint8_t max_lines, max_top_line;    // Calculate display capacity and max scroll
                         help_layout_params(total_lines, &max_lines, &max_top_line);
 
                         if(ev.key == InputKeyUp){           // Scroll up if not already at top
                             if(s.help_top_line > 0) s.help_top_line--;
                         } else if(ev.key == InputKeyDown){  // Scroll down if not at end
                             if(s.help_top_line < max_top_line) s.help_top_line++;
                         } else if(ev.key == InputKeyBack){  // Short BACK returns to menu
                             s.screen = ScreenMenu;
                         }
                     }
                 } break;
 
                 case ScreenSettings: {          // Settings interactions
                     const uint8_t ROW_TOTAL = 5;            // Total rows including header
                     const uint8_t MAX_ROWS_S = 4;           // Visible rows
 
                     if(ev.type == InputTypeShort){
                         if(ev.key == InputKeyUp){           // Move selection up (skip header)
                             if(s.cursor == 0){
                                 s.cursor = (uint8_t)(ROW_TOTAL - 1);
                                 s.first_visible =
                                     (ROW_TOTAL > MAX_ROWS_S) ? (uint8_t)(ROW_TOTAL - MAX_ROWS_S) : 0;
                             } else {
                                 s.cursor--;
                                 if(s.cursor == 2) s.cursor = 1; // Skip non-selectable header row
                                 if(s.cursor < s.first_visible) s.first_visible = s.cursor;
                             }
                         } else if(ev.key == InputKeyDown){  // Move selection down (skip header)
                             if(s.cursor == (uint8_t)(ROW_TOTAL - 1)){
                                 s.cursor = 0;
                                 s.first_visible = 0;
                             } else {
                                 s.cursor++;
                                 if(s.cursor == 2) s.cursor = 3; // Skip header
                                 if(s.cursor >= s.first_visible + MAX_ROWS_S){
                                     s.first_visible = (uint8_t)(s.cursor - (MAX_ROWS_S - 1));
                                 }
                             }
                         } else if(ev.key == InputKeyOk){    // Activate/toggle selected row
                             if(s.cursor == 0){               // Toggle "Limit run time"
                                 if(s.limit_runtime){         // Turning OFF requires warning
                                     if(show_limit_alert_confirm()){
                                         s.limit_runtime = false; // Disable limit
                                         stop_timers(&s);         // Cancel any running timers
                                         s.remaining_ms = 0;      // Clear countdown
                                     }
                                 } else {
                                     s.limit_runtime = true;  // Enable limit
                                     start_tick_timer_if_needed(&s); // Possibly start timers
                                 }
                             } else if(s.cursor == 1){        // Toggle "Arrow captcha" (placeholder)
                                 s.arrow_captcha = !s.arrow_captcha;
                             } else if(s.cursor == 3){        // Select "Embraco" inverter
                                 if(s.inverter != InvEmbraco){
                                     s.inverter = InvEmbraco; // Change selection
                                     enter_safe_menu(&s);     // Force SAFE state
                                     s.screen = ScreenMenu;   // Back to menu
                                 }
                             } else if(s.cursor == 4){        // Select "Samsung" inverter
                                 if(s.inverter != InvSamsung){
                                     s.inverter = InvSamsung; // Change selection
                                     enter_safe_menu(&s);     // Force SAFE state
                                     s.screen = ScreenMenu;   // Back to menu
                                 }
                             }
                         } else if(ev.key == InputKeyBack){  // BACK returns to main menu
                             s.screen = ScreenMenu;
                             s.cursor = 0;
                             s.first_visible = 0;
                         }
                     }
                 } break;
             } // end switch
 
             view_port_update(s.vp);             // After handling input, request a redraw
         } // end if(get queue)
     } // end while(!exit_app)
 
     /* ---------- Cleanup: return hardware and services to safe state ---------- */
     if(s.led_timer){                            // If LED timer exists
         furi_timer_stop(s.led_timer);           // -> stop
         furi_timer_free(s.led_timer);           // -> free
         s.led_timer = NULL;                     // -> clear pointer
     }
     if(s.hint_timer){                           // If hint timer exists
         furi_timer_stop(s.hint_timer);          // -> stop
         furi_timer_free(s.hint_timer);          // -> free
         s.hint_timer = NULL;                    // -> clear pointer
     }
     stop_timers(&s);                            // Stop countdown timers (if any)
     free_timers(&s);                            // Free countdown timers
     pwm_hw_stop_safe(&s.pwm_running);           // Ensure PWM is off
     pin_to_hiz();                               // Disconnect output
     power_5v_set(false);                        // Force OTG 5V OFF
     notification_message(s.notif, &sequence_reset_rgb); // Reset LEDs
     furi_record_close(RECORD_NOTIFICATION);     // Release Notification service
 
     gui_remove_view_port(s.gui, s.vp);          // Detach ViewPort from GUI
     view_port_free(s.vp);                       // Free ViewPort
     furi_message_queue_free(s.q);               // Delete input queue
     furi_record_close(RECORD_GUI);              // Release GUI service
     return 0;                                   // Normal termination code
 }