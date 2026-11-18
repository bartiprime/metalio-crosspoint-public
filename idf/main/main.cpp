// app_main is provided by the Arduino component (CONFIG_AUTOSTART_ARDUINO=y),
// which calls the sketch's setup()/loop() from upstream's src/main.cpp.
//
// This translation unit exists only so the project has a conventional main
// component and so that linking it forces the application archive in.
extern "C" void crosspoint_app_link_anchor(void);
extern "C" void app_force_link(void) { crosspoint_app_link_anchor(); }
