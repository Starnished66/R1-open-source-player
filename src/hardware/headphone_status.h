#ifndef HEADPHONE_STATUS_H
#define HEADPHONE_STATUS_H

#include <stdbool.h>

enum HEADPHONE_STATE {
	HEADPHONE_STATE_NONE,      // none plugged in
	HEADPHONE_STATE_HEADSET,   // 3.5mm plugged in
	HEADPHONE_STATE_BALANCED,  // 4.4mm plugged in
};


// returns which headphone output is plugged in
// if 3.5mm and 4.4mm are both plugged in, 4.4mm is prioritized
enum HEADPHONE_STATE get_headphone_state(void);

void headphone_status_refresh_earpods_adc(void);

#endif
