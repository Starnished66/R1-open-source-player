#ifndef HEADPHONE_STATUS_H
#define HEADPHONE_STATUS_H

#include <stdbool.h>

enum HEADPHONE_STATE {
	HEADPHONE_STATE_NONE,      // none plugged in
	HEADPHONE_STATE_HEADSET,   // 3.5mm plugged in
	HEADPHONE_STATE_BALANCED,  // 4.4mm plugged in
};

// TODO: add description
enum HEADPHONE_STATE get_headphone_state(void);

#endif
